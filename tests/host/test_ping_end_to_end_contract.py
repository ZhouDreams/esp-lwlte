#!/usr/bin/env python3
"""Static end-to-end contract checks for Ping Service implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


class PingEndToEndContractTest(unittest.TestCase):
    def assert_struct_contains_fields(self, source: str, type_name: str, fields: list[str]):
        match = re.search(
            rf"typedef\s+struct\s*\{{(?P<body>[^}}]*)\}}\s*{re.escape(type_name)}\s*;",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(match, f"missing struct definition for {type_name}")
        body = match.group("body")
        for field in fields:
            self.assertIn(field, body, f"missing {field} in {type_name}")

    def assert_has_function_prototype(self, source: str, name: str, params: list[str]):
        pattern = rf"esp_err_t\s+{re.escape(name)}\s*\((?P<params>[\s\S]*?)\)\s*;"
        match = re.search(pattern, source)
        self.assertIsNotNone(match, f"missing prototype for {name}")
        param_text = match.group("params")
        for param in params:
            self.assertRegex(param_text, param, f"missing {param} in prototype for {name}")

    def assert_function_body(self, source: str, name: str) -> str:
        match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
        self.assertIsNotNone(match, f"missing function body for {name}")
        start = match.end() - 1
        depth = 0
        for pos in range(start, len(source)):
            if source[pos] == "{":
                depth += 1
            elif source[pos] == "}":
                depth -= 1
                if depth == 0:
                    return source[start:pos + 1]
        self.fail(f"unterminated function body for {name}")

    def assert_define_at_least(self, source: str, name: str, minimum: int):
        match = re.search(rf"^\s*#\s*define\s+{re.escape(name)}\s+(?P<value>\d+)\b", source, re.MULTILINE)
        self.assertIsNotNone(match, f"missing numeric define for {name}")
        self.assertGreaterEqual(int(match.group("value")), minimum)

    def test_public_facade_api_matches_classes(self):
        lwlte_h = read("src/include/lwlte.h")
        classes_md = read("docs/agents/classes.md")

        request_fields = [
            "const char *host;",
            "uint8_t count;",
            "uint16_t data_len;",
            "uint16_t timeout_100ms;",
            "uint8_t ttl;",
            "uint32_t total_timeout_ms;",
        ]
        reply_fields = [
            "uint8_t seq;",
            "char ip[48];",
            "uint32_t time_ms;",
            "uint8_t ttl;",
            "bool success;",
        ]
        summary_fields = [
            "uint8_t sent;",
            "uint8_t received;",
            "uint8_t lost;",
            "uint32_t min_time_ms;",
            "uint32_t max_time_ms;",
            "uint32_t avg_time_ms;",
        ]
        for source in [lwlte_h, classes_md]:
            self.assert_struct_contains_fields(source, "lwlte_ping_request_t", request_fields)
            self.assert_struct_contains_fields(source, "lwlte_ping_reply_t", reply_fields)
            self.assert_struct_contains_fields(source, "lwlte_ping_summary_t", summary_fields)
            self.assert_has_function_prototype(source, "lwlte_ping", [
                r"lwlte_handle_t\s*me",
                r"const\s+lwlte_ping_request_t\s*\*\s*request",
                r"lwlte_ping_reply_t\s*\*\s*replies",
                r"size_t\s+max_replies",
                r"lwlte_ping_summary_t\s*\*\s*summary",
            ])

        self.assertNotIn("lwlte_ping_async(", lwlte_h)
        self.assertNotIn("LWLTE_EVENT_PING_DONE", lwlte_h)

    def test_ping_client_is_lightweight_and_core_only(self):
        header_path = ROOT / "src/ping_client/ping_client.h"
        priv_path = ROOT / "src/ping_client/ping_client_priv.h"
        source_path = ROOT / "src/ping_client/ping_client.c"
        self.assertTrue(header_path.exists())
        self.assertTrue(priv_path.exists())
        self.assertTrue(source_path.exists())

        header = read("src/ping_client/ping_client.h")
        priv = read("src/ping_client/ping_client_priv.h")
        source = read("src/ping_client/ping_client.c")
        combined = "\n".join([header, priv, source])

        for token in [
            "typedef struct ping_client_t *ping_client_handle_t;",
            "ping_client_handle_t ping_client_create(core_handle_t core);",
            "esp_err_t ping_client_destroy(ping_client_handle_t me);",
            "esp_err_t ping_client_ping(ping_client_handle_t me,",
            ".type = CORE_CMD_PING",
            "xSemaphoreCreateBinary()",
        ]:
            self.assertIn(token, combined)

        self.assertRegex(combined, r"\.done_cb\s*=\s*\w+")
        self.assertRegex(combined, r"\bxSemaphoreTake\s*\(")
        self.assertRegex(
            combined,
            r"CORE_CMD_PING[\s\S]*\bcore_submit_cmd\s*\([^;]+\)",
        )

        for token in [
            "active_calls",
            "active_done_sema",
            "begin_ping_call",
            "end_ping_call",
            "wait_active_calls_idle",
        ]:
            self.assertIn(token, combined)

        destroy_body = self.assert_function_body(source, "ping_client_destroy")
        ping_body = self.assert_function_body(source, "ping_client_ping")
        self.assertRegex(
            destroy_body,
            r"wait_active_calls_idle\s*\(\s*me\s*\)[\s\S]*vSemaphoreDelete\s*\([^;]*active_done_sema",
        )
        self.assertRegex(
            destroy_body,
            r"wait_active_calls_idle\s*\(\s*me\s*\)[\s\S]*free\s*\(\s*me\s*\)",
        )
        self.assertRegex(
            ping_body,
            r"begin_ping_call\s*\([^;]+\)[\s\S]*core_submit_cmd\s*\(",
        )
        self.assertRegex(
            ping_body,
            r"core_submit_cmd\s*\([^;]+\)[\s\S]*end_ping_call\s*\(\s*me\s*\)",
        )
        self.assertRegex(
            ping_body,
            r"xSemaphoreCreateBinary\s*\(\s*\)[\s\S]*end_ping_call\s*\(\s*me\s*\)",
        )

        for forbidden in [
            r'^\s*#\s*include\s+"modem\.h"',
            r'^\s*#\s*include\s+"modem_air780ep\.h"',
            r'^\s*#\s*include\s+"at_engine\.h"',
            r'^\s*#\s*include\s+"core_priv\.h"',
            r'^\s*#\s*include\s+"mqtt_client\.h"',
            r'^\s*#\s*include\s+"lwlte\.h"',
            r'^\s*#\s*include\s+"lwlte_priv\.h"',
            r'^\s*#\s*include\s+"net_mgr\.h"',
            r'^\s*#\s*include\s+"pdp_mgr\.h"',
            r"\bxTaskCreate\s*\(",
            r"\bxQueueCreate\s*\(",
            r"\besp_event_loop_create\s*\(",
            r"\bmodem_\w+\s*\(",
            r"\bat_engine_\w+\s*\(",
            r"\bmqtt_client_\w+\s*\(",
            r"\blwlte_\w+\s*\(",
            r"\bnet_mgr_\w+\s*\(",
            r"\bpdp_mgr_\w+\s*\(",
            r'"AT\+CIPPING',
        ]:
            self.assertNotRegex(combined, re.compile(forbidden, re.MULTILINE))

    def test_facade_wires_ping_and_maps_types_without_casting(self):
        lwlte_priv_h = read("src/lwlte/lwlte_priv.h")
        lwlte_c = read("src/lwlte/lwlte.c")
        air780ep_factory = read("src/lwlte/lwlte_air780ep.c")
        lwlte_ping_body = self.assert_function_body(lwlte_c, "lwlte_ping")

        for token in [
            '#include "ping_client.h"',
            "ping_client_handle_t ping;",
        ]:
            self.assertIn(token, lwlte_priv_h)

        core_create_match = re.search(r"\b\w+->core\s*=\s*core_create\s*\(", air780ep_factory)
        ping_create_match = re.search(
            r"\b\w+->ping\s*=\s*ping_client_create\s*\(\s*\w+->core\s*\)",
            air780ep_factory,
        )
        self.assertIsNotNone(core_create_match, "missing core_create assignment")
        self.assertIsNotNone(ping_create_match, "missing ping_client_create assignment")
        self.assertLess(core_create_match.start(), ping_create_match.start())
        self.assertNotIn("core_start", air780ep_factory)
        self.assertNotIn("modem_start", air780ep_factory)

        self.assertIn("ping_client_destroy(me->ping)", lwlte_c)
        self.assertRegex(lwlte_ping_body, r"ping_client_request_t\s+\w+")
        self.assertRegex(lwlte_ping_body, r"core_ping_reply_t\s*\*\s*(?P<core_replies>\w+)")
        self.assertRegex(lwlte_ping_body, r"core_ping_summary_t\s+(?P<core_summary>\w+)")
        self.assertRegex(lwlte_ping_body, r"\bping_client_ping\s*\(")

        for field in ["host", "count", "data_len", "timeout_100ms", "ttl", "total_timeout_ms"]:
            self.assertRegex(lwlte_ping_body, rf"\.{field}\s*=\s*request->{field}")

        core_replies = re.search(r"core_ping_reply_t\s*\*\s*(?P<name>\w+)", lwlte_ping_body).group("name")
        for field in ["seq", "time_ms", "ttl", "success"]:
            self.assertRegex(
                lwlte_ping_body,
                rf"replies\s*\[[^\]]+\]\.{field}\s*=\s*{core_replies}\s*\[[^\]]+\]\.{field}\s*;",
            )
        self.assertRegex(
            lwlte_ping_body,
            rf"(?:strlcpy|snprintf|memcpy|memmove|strncpy)\s*\([^;]*replies\s*\[[^\]]+\]\.ip[^;]*{core_replies}\s*\[[^\]]+\]\.ip[^;]*;",
        )

        core_summary = re.search(r"core_ping_summary_t\s+(?P<name>\w+)", lwlte_ping_body).group("name")
        for field in ["sent", "received", "lost", "min_time_ms", "max_time_ms", "avg_time_ms"]:
            self.assertRegex(
                lwlte_ping_body,
                rf"summary->{field}\s*=\s*{core_summary}\.{field}\s*;",
            )

        self.assertLess(lwlte_c.index("ping_client_destroy(me->ping)"),
                        lwlte_c.index("core_destroy(me->core)"))
        self.assertNotRegex(lwlte_ping_body, r"\(\s*core_ping_reply_t\s*\*\s*\)\s*replies")
        self.assertNotRegex(lwlte_ping_body, r"\(\s*lwlte_ping_reply_t\s*\*\s*\)\s*\w+")
        self.assertNotRegex(lwlte_ping_body, r"\(\s*(?:const\s+)?ping_client_request_t\s*\*\s*\)\s*request")
        self.assertNotRegex(lwlte_ping_body, r"\(\s*(?:const\s+)?lwlte_ping_request_t\s*\*\s*\)\s*&?\w+")
        self.assertNotRegex(lwlte_ping_body, r"\(\s*core_ping_summary_t\s*\*\s*\)\s*summary")
        self.assertNotRegex(lwlte_ping_body, r"\(\s*lwlte_ping_summary_t\s*\*\s*\)\s*&?\w+")

    def test_core_ping_command_boundary_matches_classes(self):
        core_h = read("src/core/core.h")
        core_c = read("src/core/core.c")
        core_fsm_c = read("src/core/core_fsm.c")

        for token in [
            "CORE_CMD_PING",
            "core_ping_reply_t",
            "core_ping_summary_t",
            "core_ping_reply_t *replies;",
            "size_t max_replies;",
            "core_ping_summary_t *summary;",
        ]:
            self.assertIn(token, core_h)

        for token in [
            "case CORE_CMD_PING:",
            "free((void *)cmd->data.ping.host);",
            "cmd->data.ping.host != NULL",
            "cmd->data.ping.replies != NULL",
        ]:
            self.assertIn(token, core_c)
        self.assertRegex(
            core_c,
            r"\b\w+->data\.ping\.host\s*=\s*clone_optional_string\s*\(\s*cmd->data\.ping\.host\s*\)",
        )

        for token in [
            "CORE_CMD_PING",
            "core_get_net_state",
            "CORE_NET_STATE_ONLINE",
            "ESP_ERR_INVALID_STATE",
            "modem_ping_request_t",
            "modem_ping_reply_t",
            "modem_ping_summary_t",
            "modem_ping(",
            "cmd->data.ping.replies",
            "cmd->data.ping.summary",
            "finish_service_cmd",
        ]:
            self.assertIn(token, core_fsm_c)

        allocated_modem_replies = re.search(
            r"modem_ping_reply_t\s*\*\s*(?P<name>\w+)\s*=\s*calloc\s*\(\s*cmd->data\.ping\.count\s*,\s*sizeof\s*\(\s*modem_ping_reply_t\s*\)\s*\)",
            core_fsm_c,
        )
        self.assertIsNotNone(allocated_modem_replies, "missing modem ping reply array")
        allocated_modem_replies = allocated_modem_replies.group("name")
        modem_replies = re.search(r"modem_ping_reply_t\s*\*\s*(?P<name>\w+)", core_fsm_c)
        self.assertIsNotNone(modem_replies, "missing modem ping reply array")
        modem_replies = modem_replies.group("name")
        modem_summary = re.search(r"modem_ping_summary_t\s+(?P<name>\w+)", core_fsm_c)
        self.assertIsNotNone(modem_summary, "missing modem ping summary")
        modem_summary = modem_summary.group("name")

        self.assertRegex(
            core_fsm_c,
            rf"modem_ping\s*\(\s*me->modem\s*,\s*&request\s*,\s*{allocated_modem_replies}\s*,\s*cmd->data\.ping\.count\s*,",
            "modem_ping capacity must not exceed temporary reply allocation",
        )

        for pattern in [
            r"\.host\s*=\s*cmd->data\.ping\.host",
            r"\.count\s*=\s*cmd->data\.ping\.count",
            r"\.data_len\s*=\s*cmd->data\.ping\.data_len",
            r"\.timeout_100ms\s*=\s*cmd->data\.ping\.timeout_100ms",
            r"\.ttl\s*=\s*cmd->data\.ping\.ttl",
            r"\.total_timeout_ms\s*=\s*cmd->timeout_ms",
            rf"\.seq\s*=\s*{modem_replies}\[[^\]]+\]\.seq\s*;",
            rf"\.ip\s*,\s*{modem_replies}\[[^\]]+\]\.ip",
            rf"\.time_ms\s*=\s*{modem_replies}\[[^\]]+\]\.time_ms\s*;",
            rf"\.ttl\s*=\s*{modem_replies}\[[^\]]+\]\.ttl\s*;",
            rf"\.success\s*=\s*{modem_replies}\[[^\]]+\]\.success\s*;",
            rf"->sent\s*=\s*{modem_summary}\.sent\s*;",
            rf"->received\s*=\s*{modem_summary}\.received\s*;",
            rf"->lost\s*=\s*{modem_summary}\.lost\s*;",
            rf"->min_time_ms\s*=\s*{modem_summary}\.min_time_ms\s*;",
            rf"->max_time_ms\s*=\s*{modem_summary}\.max_time_ms\s*;",
            rf"->avg_time_ms\s*=\s*{modem_summary}\.avg_time_ms\s*;",
        ]:
            self.assertRegex(core_fsm_c, pattern)

        self.assertNotRegex(core_fsm_c, r"\(\s*modem_ping_reply_t\s*\*\s*\)\s*cmd->data\.ping\.replies")
        self.assertNotRegex(core_fsm_c, rf"\(\s*core_ping_reply_t\s*\*\s*\)\s*{modem_replies}")
        self.assertNotRegex(core_fsm_c, r"\(\s*core_ping_reply_t\s*\*\s*\)\s*\w+")

    def test_modem_ping_boundary_matches_classes(self):
        modem_h = read("src/modem/modem.h")
        modem_priv_h = read("src/modem/modem_priv.h")
        modem_c = read("src/modem/modem.c")

        self.assert_struct_contains_fields(modem_h, "modem_ping_request_t", [
            "const char *host;",
            "uint8_t count;",
            "uint16_t data_len;",
            "uint16_t timeout_100ms;",
            "uint8_t ttl;",
            "uint32_t total_timeout_ms;",
        ])
        self.assert_struct_contains_fields(modem_h, "modem_ping_reply_t", [
            "uint8_t seq;",
            "char ip[48];",
            "uint32_t time_ms;",
            "uint8_t ttl;",
            "bool success;",
        ])
        self.assert_struct_contains_fields(modem_h, "modem_ping_summary_t", [
            "uint8_t sent;",
            "uint8_t received;",
            "uint8_t lost;",
            "uint32_t min_time_ms;",
            "uint32_t max_time_ms;",
            "uint32_t avg_time_ms;",
        ])
        self.assert_has_function_prototype(modem_h, "modem_ping", [
            r"modem_handle_t\s*me",
            r"const\s+modem_ping_request_t\s*\*\s*request",
            r"modem_ping_reply_t\s*\*\s*replies",
            r"size_t\s+max_replies",
            r"modem_ping_summary_t\s*\*\s*summary",
        ])

        self.assertRegex(
            modem_priv_h,
            r"typedef\s+esp_err_t\s*\(\s*\*\s*modem_ping_fn\s*\)\s*\("
            r"[\s\S]*modem_handle_t\s*me"
            r"[\s\S]*const\s+modem_ping_request_t\s*\*\s*request"
            r"[\s\S]*modem_ping_reply_t\s*\*\s*replies"
            r"[\s\S]*size_t\s+max_replies"
            r"[\s\S]*modem_ping_summary_t\s*\*\s*summary"
            r"[\s\S]*\)\s*;",
        )
        self.assertIn("modem_ping_fn ping;", modem_priv_h)
        modem_ping_body = self.assert_function_body(modem_c, "modem_ping")
        for pattern in [
            r"request->host[\s\S]*request->host\s*\[\s*0\s*\]",
            r"request->count[\s\S]*>=\s*1[\s\S]*request->count[\s\S]*<=\s*100",
            r"request->data_len[\s\S]*<=\s*1024",
            r"request->timeout_100ms[\s\S]*>=\s*1[\s\S]*request->timeout_100ms[\s\S]*<=\s*600",
            r"request->ttl[\s\S]*>=\s*1",
            r"max_replies[\s\S]*>=\s*request->count",
            r"ESP_ERR_NOT_SUPPORTED",
            r"me->ops->ping\s*\(\s*me\s*,\s*request\s*,\s*replies\s*,\s*max_replies\s*,\s*summary\s*\)",
        ]:
            self.assertRegex(modem_ping_body, pattern)

    def test_air780ep_cipping_mapping_and_parser(self):
        air780ep_c = read("src/modem/modem_air780ep.c")
        at_engine_c = read("src/at_engine/at_engine.c")

        for token in [
            "#define AIR780EP_CIPPING_PREFIX         \"+CIPPING:\"",
            "#define AIR780EP_CIPPING_MAX_COUNT      100",
            "static esp_err_t air780ep_ping(modem_handle_t me,",
            ".ping = air780ep_ping,",
            "AT+CIPPING=\"%s\",%u,%u,%u,%u",
            "parse_cipping_line",
            "calculate_ping_summary",
            "reply_time == (uint32_t)request->timeout_100ms * 100U",
            "parsed.ttl == 255",
            "parsed.success = !lost",
            "summary->sent = request->count;",
            "summary->lost = summary->sent - summary->received;",
            "ESP_ERR_INVALID_RESPONSE",
            "static esp_err_t parse_cipping_uint(const char **cursor,",
            "uint64_t total_time_ms",
        ]:
            self.assertIn(token, air780ep_c)

        air780ep_ping_body = self.assert_function_body(air780ep_c, "air780ep_ping")
        self.assertRegex(
            air780ep_ping_body,
            r"request->count[\s\S]*>=\s*1[\s\S]*request->count[\s\S]*<=\s*AIR780EP_CIPPING_MAX_COUNT",
        )
        timeout_body = self.assert_function_body(air780ep_c, "ping_cmd_timeout_ms")
        self.assertRegex(
            timeout_body,
            r"request->total_timeout_ms[\s\S]*!=\s*0[\s\S]*return\s+request->total_timeout_ms\s*;",
        )
        self.assertRegex(
            air780ep_c,
            r"_Static_assert\s*\(\s*AIR780EP_MAX_RESPONSE_LINES\s*>=\s*AIR780EP_CIPPING_MAX_COUNT\s*\+\s*1",
        )
        self.assertRegex(
            at_engine_c,
            r"_Static_assert\s*\(\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES\s*>=\s*101",
        )
        self.assert_define_at_least(air780ep_c, "AIR780EP_MAX_RESPONSE_LINES", 101)
        self.assert_define_at_least(at_engine_c, "AT_ENGINE_DEFAULT_MAX_RESP_LINES", 101)
        parse_uint_body = self.assert_function_body(air780ep_c, "parse_cipping_uint")
        self.assertRegex(parse_uint_body, r"isdigit\s*\(\s*\(\s*unsigned\s+char\s*\)\s*\*\w+")
        parse_cipping_body = self.assert_function_body(air780ep_c, "parse_cipping_line")
        self.assertRegex(
            parse_cipping_body,
            r"prefix_len\s*=\s*sizeof\s*\(\s*AIR780EP_CIPPING_PREFIX\s*\)\s*-\s*1U",
        )
        self.assertRegex(
            parse_cipping_body,
            r"strncmp\s*\(\s*line\s*,\s*AIR780EP_CIPPING_PREFIX\s*,\s*prefix_len\s*\)\s*!=\s*0",
        )
        self.assertRegex(parse_cipping_body, r"cursor\s*=\s*line\s*\+\s*prefix_len")
        self.assertRegex(parse_cipping_body, r"\*cursor\s*==\s*':'")
        self.assertNotIn("skip_prefix_value(line, AIR780EP_CIPPING_PREFIX)", parse_cipping_body)
        self.assertNotIn("reply->success = !lost;", parse_cipping_body)

        self.assertNotIn("AT+CIPPING", read("src/core/core_fsm.c"))
        self.assertNotIn("AT+CIPPING", read("src/ping_client/ping_client.c"))

    def test_at_engine_ping_budget_and_response_capacity_contract(self):
        at_engine_c = read("src/at_engine/at_engine.c")

        self.assertIn("timeout_ticks_from_ms", at_engine_c)
        self.assertIn("remaining_timeout_ticks", at_engine_c)

        send_body = self.assert_function_body(at_engine_c, "send_cmd_internal")
        self.assertRegex(send_body, r"TickType_t\s+total_timeout_ticks")
        self.assertRegex(send_body, r"TickType_t\s+start_ticks\s*=\s*xTaskGetTickCount\s*\(\s*\)")
        self.assertRegex(
            send_body,
            r"xSemaphoreTake\s*\(\s*me->cmd_mutex\s*,\s*total_timeout_ticks\s*\)",
        )
        self.assertRegex(
            send_body,
            r"remaining_timeout_ticks\s*\(\s*start_ticks\s*,\s*total_timeout_ticks\s*\)",
        )
        self.assertRegex(
            send_body,
            r"xSemaphoreTake\s*\(\s*me->cmd_done_sema\s*,\s*remaining_ticks\s*\)",
        )
        self.assertLessEqual(
            len(re.findall(r"pdMS_TO_TICKS\s*\(\s*wait_ms\s*\)", send_body)),
            1,
            "send path must not spend the full wait_ms once on cmd_mutex and again on cmd_done_sema",
        )
        write_fail_start = send_body.index("ret = write_cmd")
        wait_state_start = send_body.index("if (me->cmd_ctx == ctx)", write_fail_start)
        write_fail_path = send_body[write_fail_start:wait_state_start]
        self.assertRegex(write_fail_path, r"xSemaphoreTake\s*\(\s*me->lock\s*,\s*portMAX_DELAY\s*\)")
        self.assertRegex(write_fail_path, r"flush_rx_input_locked\s*\(\s*me\s*\)\s*;")
        self.assertRegex(write_fail_path, r"xSemaphoreGive\s*\(\s*me->lock\s*\)")

        timeout_ticks_body = self.assert_function_body(at_engine_c, "timeout_ticks_from_ms")
        self.assertRegex(timeout_ticks_body, r"pdMS_TO_TICKS\s*\(\s*timeout_ms\s*\)")
        self.assertRegex(
            timeout_ticks_body,
            r"ticks\s*==\s*0\s*&&\s*timeout_ms\s*>\s*0[\s\S]*ticks\s*=\s*1\s*;",
        )

        normalize_body = self.assert_function_body(at_engine_c, "normalize_config")
        self.assertRegex(
            normalize_body,
            r"max_response_lines\s*<=\s*0[\s\S]*AT_ENGINE_DEFAULT_MAX_RESP_LINES",
        )
        self.assertRegex(
            normalize_body,
            r"else\s+if\s*\(\s*out->runtime\.max_response_lines\s*<\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES\s*\)[\s\S]*"
            r"out->runtime\.max_response_lines\s*=\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES\s*;",
        )
        self.assertNotRegex(
            normalize_body,
            r"max_response_lines\s*>\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES[\s\S]*"
            r"max_response_lines\s*=\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES",
        )

        self.assertRegex(
            at_engine_c,
            r"_Static_assert\s*\(\s*AT_ENGINE_DEFAULT_MAX_RESP_LINES\s*>=\s*101",
        )
        self.assert_define_at_least(at_engine_c, "AT_ENGINE_DEFAULT_MAX_RESP_LINES", 101)

    def test_at_engine_response_pool_allocates_lines_on_demand(self):
        at_engine_c = read("src/at_engine/at_engine.c")

        struct_match = re.search(r"struct\s+at_engine_t\s*\{(?P<body>[\s\S]*?)\n\};",
                                 at_engine_c)
        self.assertIsNotNone(struct_match, "missing at_engine_t struct")
        struct_body = struct_match.group("body")
        self.assertIn("char **response_pool;", struct_body)
        self.assertNotIn("char *response_pool;", struct_body)

        init_body = self.assert_function_body(at_engine_c, "init_resources")
        self.assertRegex(init_body, r"calloc\s*\(\s*\(\s*size_t\s*\)\s*me->response_pool_lines\s*,\s*sizeof\s*\(\s*me->response_pool\s*\[\s*0\s*\]\s*\)\s*\)")
        self.assertNotIn("(size_t)me->response_pool_lines, (size_t)me->response_line_size", init_body)

        append_body = self.assert_function_body(at_engine_c, "append_response_line_locked")
        self.assertIn("strnlen(line, (size_t)me->response_line_size - 1U)", append_body)
        self.assertIn("malloc(copy_len + 1U)", append_body)
        self.assertIn("abort_current_cmd_for_no_mem_locked(me, ctx);", append_body)
        self.assertIn("ctx->response->lines[ctx->data_line_index] = dst;", append_body)

        clear_body = self.assert_function_body(at_engine_c, "clear_response_pool")
        self.assertIn("for (int i = 0; i < me->response_pool_lines; i++)", clear_body)
        self.assertIn("free(me->response_pool[i]);", clear_body)
        self.assertIn("me->response_pool[i] = NULL;", clear_body)

        final_body = self.assert_function_body(at_engine_c, "append_final_response_line_locked")
        self.assertIn("free(me->response_pool[limit - 1]);", final_body)
        self.assertIn("abort_current_cmd_for_no_mem_locked(me, ctx);", final_body)
        self.assertNotIn("me->response_pool + ((size_t)(limit - 1)", final_body)

        abort_body = self.assert_function_body(at_engine_c,
                                               "abort_current_cmd_for_no_mem_locked")
        self.assertIn("ctx->io_error = ESP_ERR_NO_MEM;", abort_body)
        self.assertIn("flush_rx_input_locked(me);", abort_body)
        self.assertIn("finish_cmd_locked(me, AT_RESP_ERROR, 0);", abort_body)

    def test_cmake_registers_ping_client_source(self):
        cmake = read("src/CMakeLists.txt")
        self.assertIn('"ping_client/ping_client.c"', cmake)
        match = re.search(r"PRIV_INCLUDE_DIRS(?P<body>.*?)(?:\n\s*[A-Z_]+|\))", cmake, re.DOTALL)
        self.assertIsNotNone(match, "missing PRIV_INCLUDE_DIRS")
        self.assertRegex(match.group("body"), r"\bping_client\b")

    def test_classes_doc_and_source_keep_ping_boundary_aligned(self):
        classes_md = read("docs/agents/classes.md")
        for rel_path in [
            "src/include/lwlte.h",
            "src/ping_client/ping_client.h",
            "src/core/core.h",
            "src/modem/modem.h",
        ]:
            source = read(rel_path)
            for token in [
                "lwlte_ping_request_t" if rel_path.endswith("lwlte.h") else None,
                "ping_client_request_t" if rel_path.endswith("ping_client.h") else None,
                "CORE_CMD_PING" if rel_path.endswith("core.h") else None,
                "modem_ping_request_t" if rel_path.endswith("modem.h") else None,
            ]:
                if token:
                    self.assertIn(token, source)
                    self.assertIn(token, classes_md)


if __name__ == "__main__":
    unittest.main()
