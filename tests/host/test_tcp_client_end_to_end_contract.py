#!/usr/bin/env python3
"""Static end-to-end contract checks for TCP client v1."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read_optional(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class TcpClientEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional("src/include/lwlte.h")
        cls.lwlte_priv = read_optional("src/lwlte/lwlte_priv.h")
        cls.lwlte_c = read_optional("src/lwlte/lwlte.c")
        cls.core_h = read_optional("src/core/core.h")
        cls.core_priv = read_optional("src/core/core_priv.h")
        cls.core_c = read_optional("src/core/core.c")
        cls.core_fsm_c = read_optional("src/core/core_fsm.c")
        cls.modem_h = read_optional("src/modem/modem.h")
        cls.modem_priv = read_optional("src/modem/modem_priv.h")
        cls.modem_c = read_optional("src/modem/modem.c")
        cls.air780ep_c = read_optional("src/modem/modem_air780ep.c")
        cls.lwlte_air780ep_c = read_optional("src/lwlte/lwlte_air780ep.c")
        cls.ml307r_h = read_optional("src/modem/modem_ml307r.h")
        cls.ml307r_c = read_optional("src/modem/modem_ml307r.c")
        cls.lwlte_ml307r_c = read_optional("src/lwlte/lwlte_ml307r.c")
        cls.mqtt_c = read_optional("src/mqtt_client/mqtt_client.c")
        cls.tcp_h = read_optional("src/tcp_client/tcp_client.h")
        cls.tcp_priv = read_optional("src/tcp_client/tcp_client_priv.h")
        cls.tcp_c = read_optional("src/tcp_client/tcp_client.c")
        cls.src_cmake = read_optional("src/CMakeLists.txt")
        cls.example_h = read_optional("example/example.h")
        cls.example_main = read_optional("example/main.c")
        cls.example_cmake = read_optional("example/CMakeLists.txt")
        cls.example_kconfig = read_optional("example/Kconfig.projbuild")
        cls.air_example = read_optional("example/air780ep_tcp_client.c")
        cls.ml_example = read_optional("example/ml307r_tcp_client.c")
        cls.classes_md = read_optional("docs/agents/classes.md")
        cls.arch_md = read_optional("docs/agents/architecture.md")
        cls.roadmap_md = read_optional("docs/agents/feature-roadmap.md")

    def assert_contains_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

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

    def assert_ordered(self, text: str, tokens: list[str], label: str):
        pos = -1
        for token in tokens:
            next_pos = text.find(token, pos + 1)
            self.assertNotEqual(next_pos, -1, f"{label} missing ordered token {token}")
            pos = next_pos

    def test_public_tcp_api_exists(self):
        self.assert_contains_all(self.lwlte_h, [
            "typedef struct lwlte_tcp_conn lwlte_tcp_conn_t;",
            "LWLTE_TCP_CONN_STATE_CREATED",
            "LWLTE_TCP_CONN_STATE_CONNECTED",
            "LWLTE_TCP_EVENT_CONNECTED",
            "LWLTE_TCP_EVENT_DATA",
            "ESP_EVENT_DECLARE_BASE(LWLTE_TCP_EVENT)",
            "lwlte_tcp_config_t",
            "lwlte_tcp_open_config_t",
            "lwlte_tcp_event_data_t",
            "owns_event",
            "esp_err_t lwlte_tcp_init(lwlte_handle_t *me,",
            "esp_err_t lwlte_tcp_open(lwlte_handle_t *me,",
            "esp_err_t lwlte_tcp_send(lwlte_tcp_conn_t *conn,",
            "esp_err_t lwlte_tcp_close(lwlte_tcp_conn_t *conn);",
            "esp_err_t lwlte_tcp_conn_destroy(lwlte_tcp_conn_t *conn);",
            "void lwlte_tcp_event_data_release(lwlte_tcp_event_data_t *data);",
        ], "lwlte.h")
        self.assertIn("ESP_EVENT_DEFINE_BASE(LWLTE_TCP_EVENT)", self.lwlte_c)
        release_body = self.assert_function_body(self.lwlte_c, "lwlte_tcp_event_data_release")
        self.assertIn("data->owns_payload", release_body)
        self.assertIn("free((void *)data->payload)", release_body)
        self.assertIn("data->owns_event", release_body)
        self.assertIn("tcp_client_conn_release_event", release_body)

    def test_tcp_service_layer_boundary_and_cmake(self):
        self.assertIn('"tcp_client/tcp_client.c"', self.src_cmake)
        self.assertRegex(self.src_cmake, r"PRIV_INCLUDE_DIRS[\s\S]*\btcp_client\b")
        self.assert_contains_all(self.tcp_h + self.tcp_priv + self.tcp_c, [
            "typedef struct tcp_client_handle tcp_client_handle_t;",
            "typedef struct tcp_client_conn tcp_client_conn_t;",
            "tcp_client_create",
            "tcp_client_open",
            "tcp_client_send",
            "tcp_client_close",
            "tcp_client_conn_destroy",
            "TCP_CLIENT_DEFAULT_MAX_RX_EVENT_LEN",
            "TCP_SIG_PROTOCOL_DATA",
            "TCP_SIG_PROTOCOL_CLOSED",
            "CORE_CMD_SOCKET_OPEN",
            "CORE_CMD_SOCKET_SEND",
            "CORE_CMD_SOCKET_RECV",
            "CORE_CMD_SOCKET_CLOSE",
            "core_register_protocol_callback(core, CORE_PROTOCOL_TCP",
            "core_register_protocol_closed_callback(core, CORE_PROTOCOL_TCP",
        ], "tcp service")
        self.assertNotIn("ret != ESP_OK && ret != ESP_ERR_TIMEOUT", self.tcp_c)
        self.assert_contains_all(self.tcp_priv + self.tcp_c, [
            "send_queue_lock",
            "active_refs",
            "active_done_sema",
            "remote_closed",
            "acquire_conn",
            "release_conn",
            "latch_remote_closed",
            "conn still exists",
            "conn_can_submit",
            "send_fsm_sig_wait",
        ], "tcp lifetime safeguards")
        for forbidden in [
            '#include "modem.h"',
            '#include "modem_air780ep.h"',
            '#include "modem_ml307r.h"',
            '#include "at_engine.h"',
            '#include "core_priv.h"',
        ]:
            self.assertNotIn(forbidden, self.tcp_h + self.tcp_priv + self.tcp_c)

    def test_tcp_service_layer_boundary_and_cmake_lifetime_fences(self):
        destroy_body = self.assert_function_body(self.tcp_c, "tcp_client_conn_destroy")
        client_destroy_body = self.assert_function_body(self.tcp_c, "tcp_client_destroy")
        core_done_body = self.assert_function_body(self.tcp_c, "tcp_core_cmd_done_cb")
        post_event_start = self.tcp_c.rindex("static esp_err_t post_tcp_event")
        post_event_end = self.tcp_c.rindex("static void post_error_event")
        post_event_body = self.tcp_c[post_event_start:post_event_end]
        send_ready_body = self.assert_function_body(self.tcp_c, "handle_send_ready")
        handle_signal_body = self.assert_function_body(self.tcp_c, "handle_signal")
        protocol_closed_body = self.assert_function_body(self.tcp_c, "tcp_protocol_closed_cb")
        net_offline_body = self.assert_function_body(self.tcp_c, "handle_lwlte_event")
        fsm_protocol_closed_body = self.assert_function_body(self.tcp_c, "handle_protocol_closed")
        open_body = self.assert_function_body(self.tcp_c, "tcp_client_open")
        send_body = self.assert_function_body(self.tcp_c, "tcp_client_send")
        close_body = self.assert_function_body(self.tcp_c, "tcp_client_close")

        with self.subTest("client_destroy rejects any live conn"):
            self.assert_contains_all(client_destroy_body, [
                "if (me->conn)",
                "if (me->deferred_destroy_conn)",
                "conn still exists",
            ], "tcp_client_destroy terminal conn cleanup")
            self.assertNotIn("conn->pending_cmd.active", client_destroy_body)
            self.assertIn("conn->pending_cmd.active", destroy_body)
            self.assertNotIn("handle_remote_closed_if_latched", client_destroy_body)
            self.assertNotIn("post_tcp_event", client_destroy_body)
            self.assertNotIn("handle_event_release", client_destroy_body)
            self.assertIn("me->deferred_destroy_conn", open_body)

        with self.subTest("conn_destroy does not queue conn payload before free"):
            self.assertNotIn("handle_remote_closed_if_latched(client, conn)", destroy_body)
            self.assertNotIn("LWLTE_TCP_EVENT_DISCONNECTED", destroy_body)

        with self.subTest("callbacks only enqueue close signals; FSM latches close state"):
            self.assertNotIn("latch_remote_closed", protocol_closed_body)
            self.assertNotIn("latch_remote_closed", net_offline_body)
            self.assertNotIn("clear_pending_cmd_if_matches", core_done_body)
            self.assertIn("latch_remote_closed", fsm_protocol_closed_body)

        with self.subTest("posted events hold conn refs until event loop drains"):
            self.assert_contains_all(self.tcp_priv + self.tcp_c, [
                "event_payload.owns_event = true",
                "tcp_client_conn_release_event",
                "conn->active_refs > 0",
            ], "tcp event lifetime")
            self.assertNotIn("wait_conn_idle(conn)", destroy_body)
            self.assert_ordered(post_event_body, [
                "acquire_conn(conn)",
                "event_payload.owns_event = true",
                "esp_event_post",
            ], "post_tcp_event conn ref")

        with self.subTest("core completions are not dropped on a full FSM queue"):
            self.assertIn("send_fsm_sig_wait", self.tcp_c)
            self.assertIn("portMAX_DELAY", core_done_body)
            self.assertIn("portMAX_DELAY", protocol_closed_body)
            self.assertIn("portMAX_DELAY", net_offline_body)

        with self.subTest("close requests take priority over queued sends"):
            self.assert_ordered(send_ready_body, [
                "close_requested",
                "handle_close(me, conn)",
                "xQueueReceive(conn->send_queue",
            ], "close before send dequeue")
            self.assertIn("handle_close(me, conn)", send_ready_body)

        with self.subTest("send and close publish mutations only after signal enqueue"):
            self.assert_ordered(send_body, [
                "xQueueSend(me->fsm_queue, &sig, 0)",
                "xQueueSend(conn->send_queue",
            ], "tcp_client_send signal before queue publish")
            self.assert_ordered(close_body, [
                "xQueueSend(me->fsm_queue, &sig, 0)",
                "conn->close_requested = true",
            ], "tcp_client_close signal before state publish")

        with self.subTest("queued connection signals carry generation fences"):
            self.assert_contains_all(self.tcp_priv + self.tcp_c, [
                "uint32_t next_conn_generation;",
                "uint32_t generation;",
                "bool conn_scoped;",
                "uint32_t conn_generation;",
                "signal_matches_current_conn",
            ], "tcp generation fence")
            self.assertIn("conn_scoped", protocol_closed_body)
            self.assertIn("conn_generation", protocol_closed_body)
            self.assertIn("conn_scoped", net_offline_body)
            self.assertIn("conn_generation", net_offline_body)
            self.assert_ordered(handle_signal_body, [
                "signal_matches_current_conn(me, sig, &conn)",
                "switch (sig->type)",
            ], "handle_signal generation fence")

        with self.subTest("send and close state changes are atomic with terminal latch"):
            self.assertNotIn("get_conn_state_value(conn) != TCP_CONN_STATE_CONNECTED", send_body)
            self.assert_ordered(send_body, [
                "xSemaphoreTake(conn->lock",
                "conn->state != TCP_CONN_STATE_CONNECTED",
                "xQueueSend(conn->send_queue",
                "xSemaphoreGive(conn->lock",
            ], "tcp_client_send atomic state and queue")
            self.assertNotIn("tcp_conn_state_t state = get_conn_state_value(conn);", close_body)
            self.assert_ordered(close_body, [
                "xSemaphoreTake(conn->lock",
                "conn->state != TCP_CONN_STATE_CONNECTED",
                "conn->close_requested = true",
                "conn->state = TCP_CONN_STATE_CLOSING",
                "xSemaphoreGive(conn->lock",
            ], "tcp_client_close atomic state transition")

    def test_tcp_events_retain_conn_until_event_data_release(self):
        post_event_start = self.tcp_c.rindex("static esp_err_t post_tcp_event")
        post_event_end = self.tcp_c.rindex("static void post_error_event")
        post_event_body = self.tcp_c[post_event_start:post_event_end]

        self.assert_ordered(post_event_body, [
            "if (!ref_acquired && !acquire_conn(conn))",
            "if (payload->owns_payload)",
            "return ESP_ERR_INVALID_STATE;",
            "event_payload.owns_event = true",
        ], "TCP events hold conn refs until event release")
        self.assertNotIn("bool retain_conn = payload->owns_payload", post_event_body)
        self.assertNotIn("event_payload.owns_event = retain_conn", post_event_body)

    def test_remote_close_event_waits_until_pending_socket_command_releases_ref(self):
        remote_closed_body = self.assert_function_body(self.tcp_c,
                                                       "handle_remote_closed_if_latched")
        core_done_body = self.assert_function_body(self.tcp_c,
                                                   "handle_core_cmd_done")
        protocol_closed_body = self.assert_function_body(self.tcp_c,
                                                         "handle_protocol_closed")

        self.assert_ordered(remote_closed_body, [
            "conn->pending_cmd.active",
            "conn->remote_closed_event_pending = true",
            "return;",
            "if (post_pending_terminal_event(conn) != ESP_OK)",
            "conn->remote_closed_event_pending = true;",
            "return;",
        ], "remote close event is deferred while a socket command is pending")
        self.assert_ordered(self.assert_function_body(self.tcp_c, "latch_remote_closed"), [
            "if (conn->terminal_event_posted)",
            "conn->remote_closed_event_posted = true;",
            "mark_terminal_event_pending(conn, LWLTE_TCP_EVENT_DISCONNECTED",
        ], "duplicate remote close after terminal event is not destroy-blocking")
        self.assert_ordered(core_done_body, [
            "conn->pending_cmd.active = false;",
            "if (current_state == TCP_CONN_STATE_CLOSED)",
            "handle_remote_closed_if_latched(me, conn);",
        ], "socket completion posts deferred remote close after clearing pending command")
        self.assertIn("remote_closed_event_pending", self.tcp_priv)
        self.assertIn("handle_remote_closed_if_latched(me, conn)", protocol_closed_body)
        self.assertNotIn("conn->active_refs > 0", remote_closed_body)

    def test_terminal_conn_destroy_defers_cleanup_until_active_refs_drain(self):
        destroy_body = self.assert_function_body(self.tcp_c,
                                                 "tcp_client_conn_destroy")
        release_body = self.assert_function_body(self.tcp_c, "release_conn")
        terminal_start = self.tcp_c.rindex("static esp_err_t post_pending_terminal_event")
        terminal_end = self.tcp_c.rindex("static void post_error_event")
        terminal_body = self.tcp_c[terminal_start:terminal_end]
        client_destroy_body = self.assert_function_body(self.tcp_c,
                                                        "tcp_client_destroy")

        self.assertNotIn("if (conn->active_refs > 0)", destroy_body)
        self.assertIn("tcp_client_conn_t *deferred_destroy_conn;", self.tcp_priv)
        self.assert_ordered(destroy_body, [
            "xSemaphoreTake(client->lock",
            "xSemaphoreTake(conn->lock",
        ], "conn_destroy follows client-before-conn lock order")
        self.assert_ordered(destroy_body, [
            "if (conn->pending_cmd.active)",
            "return ESP_ERR_INVALID_STATE;",
            "if (conn->terminal_event_pending ||",
            "return ESP_ERR_INVALID_STATE;",
            "conn->destroyed = true;",
            "bool cleanup_now = conn->active_refs == 0;",
            "client->deferred_destroy_conn = cleanup_now ? NULL : conn;",
            "client->conn = NULL;",
            "if (cleanup_now) {",
            "cleanup_conn(conn);",
        ], "terminal destroy unlinks immediately and frees only when idle")
        self.assert_ordered(release_body, [
            "bool cleanup_now = false;",
            "tcp_client_handle_t *client = conn->client;",
            "conn->active_refs--;",
            "cleanup_now = conn->destroyed && conn->active_refs == 0;",
            "if (cleanup_now && client && client->lock)",
            "client->deferred_destroy_conn = NULL;",
            "if (cleanup_now) {",
            "cleanup_conn(conn);",
        ], "last active ref frees a destroyed terminal conn")
        self.assertIn("me->deferred_destroy_conn", client_destroy_body)
        self.assert_contains_all(self.tcp_priv, [
            "bool terminal_event_pending;",
            "bool terminal_event_posted;",
            "int terminal_event_id;",
            "esp_err_t terminal_error_code;",
        ], "terminal event state")
        self.assert_ordered(terminal_body, [
            "conn->active_refs++;",
            "conn->terminal_event_posted = true;",
            "conn->terminal_event_pending = false;",
            "conn->remote_closed_event_posted = true;",
            "post_tcp_event_with_ref(conn, event_id, &payload, true, false)",
            "if (ret != ESP_OK)",
            "conn->terminal_event_posted = false;",
            "conn->terminal_event_pending = true;",
            "release_conn(conn);",
        ], "terminal event post state")
        self.assert_ordered(self.tcp_c[self.tcp_c.rindex("static esp_err_t post_tcp_event_with_ref"):
                                       self.tcp_c.rindex("static void mark_terminal_event_pending")], [
            "lwlte_tcp_event_data_t empty_payload = {0};",
            "if (!payload)",
            "get_conn_state_value(conn)",
        ], "empty payload only reads conn state when needed")

    def test_air780ep_facade_default_line_buffer_covers_default_tcp_hex_rx(self):
        init_body = self.assert_function_body(self.lwlte_air780ep_c,
                                              "lwlte_air780ep_init")

        self.assertIn("LWLTE_AIR780EP_DEFAULT_AT_LINE_BUF_SIZE",
                      self.lwlte_air780ep_c)
        self.assertRegex(self.lwlte_air780ep_c,
                         r"#define LWLTE_AIR780EP_DEFAULT_AT_LINE_BUF_SIZE\s+2048")
        self.assert_ordered(init_body, [
            "int at_rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?",
            "LWLTE_AIR780EP_DEFAULT_AT_LINE_BUF_SIZE",
            ".rx_line_buf_size = at_rx_line_buf_size,",
        ], "air780ep facade line buffer default")

    def test_facade_owns_tcp_and_destroys_before_core(self):
        self.assertIn('#include "tcp_client.h"', self.lwlte_priv)
        self.assertIn("tcp_client_handle_t *tcp;", self.lwlte_priv)
        self.assert_contains_all(self.lwlte_c, [
            "static lwlte_tcp_conn_state_t map_tcp_conn_state",
            "tcp_client_create(&tcp_config, core)",
            "tcp_client_open(tcp, &open_config",
            "tcp_client_send((tcp_client_conn_t *)conn",
            "tcp_client_close((tcp_client_conn_t *)conn)",
            "tcp_client_conn_destroy((tcp_client_conn_t *)conn)",
        ], "lwlte.c")
        destroy_body = self.lwlte_c[self.lwlte_c.rindex("static esp_err_t destroy_owned_resources"):]
        self.assertIn("tcp_client_destroy(me->tcp)", destroy_body)
        self.assertLess(destroy_body.index("tcp_client_destroy(me->tcp)"), destroy_body.index("core_destroy(me->core)"))

    def test_tcp_facade_serializes_service_pointer_lifetime(self):
        init_body = self.assert_function_body(self.lwlte_c, "lwlte_tcp_init")
        destroy_body = self.assert_function_body(self.lwlte_c, "lwlte_tcp_destroy")
        open_body = self.assert_function_body(self.lwlte_c, "lwlte_tcp_open")

        self.assert_ordered(init_body, [
            "xSemaphoreTake(me->lock",
            "tcp_client_create(&tcp_config, core)",
            "me->tcp = tcp",
            "xSemaphoreGive(me->lock",
        ], "lwlte_tcp_init serialization")
        self.assert_ordered(destroy_body, [
            "xSemaphoreTake(me->lock",
            "tcp_client_destroy(tcp)",
            "me->tcp = NULL",
            "xSemaphoreGive(me->lock",
        ], "lwlte_tcp_destroy serialization")
        self.assert_ordered(open_body, [
            "xSemaphoreTake(me->lock",
            "tcp = me->tcp",
            "tcp_client_open(tcp, &open_config",
            "xSemaphoreGive(me->lock",
        ], "lwlte_tcp_open serialization")

    def test_core_protocol_callbacks_are_protocol_indexed(self):
        self.assert_contains_all(self.core_h + self.core_priv + self.core_c, [
            "CORE_PROTOCOL_MQTT",
            "CORE_PROTOCOL_TCP",
            "CORE_PROTOCOL_MAX",
            "uint8_t conn_id;",
            "int reason;",
            "int modem_error_code;",
            "protocol_callbacks[CORE_PROTOCOL_MAX]",
            "protocol_closed_callbacks[CORE_PROTOCOL_MAX]",
            "protocol_callback_active[CORE_PROTOCOL_MAX]",
            "protocol_callback_reg_lock",
            "protocol_callback_done_sema",
            "protocol_closed_callback_active[CORE_PROTOCOL_MAX]",
            "protocol_closed_callback_reg_lock",
            "protocol_closed_callback_done_sema",
            "core_register_protocol_callback(core_handle_t *me,",
            "core_protocol_t protocol,",
            "const core_protocol_data_t *data,",
            "core_register_protocol_closed_callback(core_handle_t *me,",
            "core_fsm_is_task(me)",
        ], "core protocol routing")
        self.assertIn("xSemaphoreCreateMutex()", self.core_c)
        self.assertIn("xSemaphoreTake(me->protocol_callback_reg_lock", self.core_c)
        self.assertIn("xSemaphoreGive(me->protocol_callback_reg_lock", self.core_c)
        self.assertIn("xSemaphoreTake(me->protocol_closed_callback_reg_lock", self.core_c)
        self.assertIn("xSemaphoreGive(me->protocol_closed_callback_reg_lock", self.core_c)
        self.assertIn("xSemaphoreTake(done_sema, portMAX_DELAY)", self.core_c)
        self.assertIn("xSemaphoreGive(done_sema)", self.core_fsm_c)
        self.assertIn("core_register_protocol_callback(core, CORE_PROTOCOL_MQTT", self.mqtt_c)
        self.assertIn("core_register_protocol_closed_callback(core, CORE_PROTOCOL_MQTT", self.mqtt_c)
        self.assertNotIn("core_register_protocol_callback(core, mqtt_protocol_data_cb, me)", self.mqtt_c)

    def test_tcp_protocol_closed_reason_propagates_to_user_event(self):
        core_closed_body = self.assert_function_body(self.core_fsm_c, "handle_modem_event")
        tcp_closed_cb_body = self.assert_function_body(self.tcp_c, "tcp_protocol_closed_cb")
        tcp_handle_closed_body = self.assert_function_body(self.tcp_c,
                                                          "handle_protocol_closed")

        self.assert_contains_all(self.core_h + self.core_fsm_c + self.tcp_c, [
            "const core_protocol_data_t *data,",
            "cb(me, protocol, &pd, ctx);",
            "data->reason",
            "data->modem_error_code",
            "sig->error_code",
            "sig->modem_error_code",
            "latch_remote_closed(conn, sig->error_code, sig->modem_error_code)",
        ], "TCP protocol close reason propagation")
        self.assert_ordered(core_closed_body, [
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "core_protocol_data_t pd = {",
            ".reason           = event->data.protocol_data.reason,",
            ".modem_error_code = event->data.protocol_data.modem_error_code,",
            "cb(me, protocol, &pd, ctx);",
        ], "core closed callback data")
        self.assertNotIn("cb(me, protocol, ctx);", core_closed_body)
        self.assertNotIn("latch_remote_closed(conn, 0, 0)", tcp_handle_closed_body)
        self.assertIn("sig.error_code = data->reason;", tcp_closed_cb_body)
        self.assertIn("sig.modem_error_code = data->modem_error_code;",
                      tcp_closed_cb_body)

    def test_tcp_core_command_errors_preserve_esp_and_modem_codes(self):
        core_h = self.core_h
        core_fsm_body = self.assert_function_body(self.core_fsm_c,
                                                  "handle_service_cmd")
        tcp_done_body = self.assert_function_body(self.tcp_c,
                                                  "tcp_core_cmd_done_cb")
        tcp_handle_done_body = self.assert_function_body(self.tcp_c,
                                                         "handle_core_cmd_done")

        self.assert_contains_all(core_h + self.core_fsm_c + self.tcp_c, [
            "core_socket_result_t",
            ".error_code = ret",
            ".modem_error_code = modem_error_code",
            "socket_cmd && ret != ESP_OK ? (const void *)&result",
            "socket_result->error_code",
            "socket_result->modem_error_code",
            "esp_err_from_core_result(sig->core_result)",
            "post_error_event(conn, error_code, sig->modem_error_code, 0)",
        ], "TCP Core command error propagation")
        self.assert_ordered(tcp_done_body, [
            "sig.error_code = esp_err_from_core_result(result);",
            "const core_socket_result_t *socket_result = result_data;",
            "sig.error_code = socket_result->error_code;",
            "sig.modem_error_code = socket_result->modem_error_code;",
        ], "tcp done copies socket error details")
        self.assert_ordered(tcp_handle_done_body, [
            "esp_err_t error_code = sig->error_code ?",
            "esp_err_from_core_result(sig->core_result);",
            "post_error_event(conn, error_code, sig->modem_error_code, 0);",
        ], "tcp error event uses detailed error")
        self.assertNotIn("post_error_event(conn, ESP_FAIL, 0, 0);",
                         tcp_handle_done_body)
        self.assertIn("CORE_CMD_SOCKET_RECV", core_fsm_body)
        self.assertIn("socket_cmd && ret != ESP_OK ? (const void *)&result",
                      core_fsm_body)

    def test_air780ep_send_close_failures_preserve_modem_error_code(self):
        core_fsm_body = self.assert_function_body(self.core_fsm_c,
                                                  "handle_service_cmd")
        send_body = self.assert_function_body(self.air780ep_c,
                                              "air780ep_socket_send")
        close_body = self.assert_function_body(self.air780ep_c,
                                               "air780ep_socket_close")

        self.assert_contains_all(self.modem_h, [
            "int *modem_error_code;        /**< 模块原始错误码输出，可为 NULL； Raw modem error code output, optional */",
        ], "modem socket error outputs")
        self.assert_ordered(core_fsm_body, [
            "modem_socket_send_t request = {",
            ".modem_error_code = &modem_error_code,",
            "ret = modem_socket_send(me->modem, &request);",
        ], "core passes modem error output for socket send")
        self.assert_ordered(core_fsm_body, [
            "modem_socket_close_t request = {",
            ".modem_error_code = &modem_error_code,",
            "ret = modem_socket_close(me->modem, &request);",
        ], "core passes modem error output for socket close")
        self.assert_ordered(send_body, [
            "if (send->modem_error_code)",
            "*send->modem_error_code = ctx.response.error_code;",
            "return ret;",
        ], "Air780EP CIPSEND propagates AT error code")
        self.assert_ordered(close_body, [
            "if (close->modem_error_code)",
            "*close->modem_error_code = ctx.response.error_code;",
            "return ret;",
        ], "Air780EP CIPCLOSE propagates AT error code")

    def test_tcp_recv_failure_result_is_not_freed_as_payload(self):
        tcp_done_body = self.assert_function_body(self.tcp_c,
                                                  "tcp_core_cmd_done_cb")

        self.assertIn("type == CORE_CMD_SOCKET_RECV && result == CORE_CMD_RESULT_OK && result_data",
                      tcp_done_body)
        self.assertNotIn("type == CORE_CMD_SOCKET_RECV && result_data)",
                         tcp_done_body)

    def test_core_socket_commands_exist_and_clone_ownership(self):
        self.assert_contains_all(self.core_h + self.core_c + self.core_fsm_c, [
            "CORE_SOCKET_PROTO_TCP",
            "core_socket_open_t",
            "core_socket_send_t",
            "core_socket_recv_t",
            "core_socket_recv_result_t",
            "CORE_CMD_SOCKET_OPEN",
            "CORE_CMD_SOCKET_SEND",
            "CORE_CMD_SOCKET_RECV",
            "CORE_CMD_SOCKET_CLOSE",
            "modem_socket_open",
            "modem_socket_send",
            "modem_socket_recv",
            "modem_socket_close",
            "clone_payload(cmd->data.socket_send.data",
            "free((void *)cmd->data.socket_open.host)",
            "free((void *)cmd->data.socket_send.data)",
            "CORE_NET_STATE_ONLINE",
            "ESP_ERR_INVALID_STATE",
        ], "core socket commands")
        self.assertIn("cmd->type == CORE_CMD_SOCKET_RECV && !cmd->done_cb", self.core_c)

    def test_modem_socket_api_exists(self):
        self.assert_contains_all(self.modem_h + self.modem_priv + self.modem_c, [
            "MODEM_PROTOCOL_TCP",
            "MODEM_SOCKET_PROTO_TCP",
            "modem_socket_open_t",
            "modem_socket_send_t",
            "modem_socket_recv_t",
            "modem_socket_recv_result_t",
            "modem_socket_close_t",
            "esp_err_t modem_socket_open(modem_handle_t *me,",
            "esp_err_t modem_socket_send(modem_handle_t *me,",
            "esp_err_t modem_socket_recv(modem_handle_t *me,",
            "esp_err_t modem_socket_close(modem_handle_t *me,",
            "modem_socket_open_fn socket_open;",
            "modem_socket_send_fn socket_send;",
            "modem_socket_recv_fn socket_recv;",
            "modem_socket_close_fn socket_close;",
        ], "modem socket api")

    def test_air780ep_tcp_mapping_tokens(self):
        self.assert_contains_all(self.air780ep_c, [
            "AT+CIPMUX=0",
            "AT+CIPMODE=0",
            "AT+CIPQSEND=1",
            "AT+CIPRXF=1",
            "AT+CIPRXGET=5",
            "AT+CIPSTART=\"TCP\",\"%s\",%u",
            "AT+CIPSEND=%u",
            "at_engine_send_cmd_with_payload",
            "AT+CIPRXGET=3,%u",
            "AIR780EP_TCP_MAX_HEX_READ_BYTES",
            "CONNECT OK",
            "ALREADY CONNECT",
            "DATAACCEPT",
            "SEND OK",
            "CLOSE OK",
            "MODEM_PROTOCOL_TCP",
            ".socket_open = air780ep_socket_open",
            ".socket_send = air780ep_socket_send",
            ".socket_recv = air780ep_socket_recv",
            ".socket_close = air780ep_socket_close",
        ], "air780ep tcp mapping")

    def test_air780ep_tcpip_settings_are_configured_before_activation(self):
        activate_body = self.assert_function_body(self.air780ep_c,
                                                  "air780ep_activate_pdp")
        open_body = self.assert_function_body(self.air780ep_c,
                                             "air780ep_socket_open")
        prepare_body = self.assert_function_body(self.air780ep_c,
                                                "air780ep_socket_prepare")

        self.assert_ordered(prepare_body, [
            "AT+CIPMUX=0",
            "AT+CIPMODE=0",
            "AT+CIPQSEND=1",
            "AT+CIPRXF=1",
            "AT+CIPRXGET=5",
        ], "air780ep TCPIP socket options")
        self.assert_ordered(activate_body, [
            "air780ep_socket_prepare(self)",
            "cstt_cmd",
            "AT+CIICR",
            "AT+CIFSR",
        ], "air780ep CIPMUX before TCPIP activation")
        self.assertNotIn("air780ep_socket_prepare(self)", open_body)

    def test_air780ep_tcp_readable_urc_accepts_spaced_and_compact_forms(self):
        register_body = self.assert_function_body(self.air780ep_c, "register_urcs")
        unregister_body = self.assert_function_body(self.air780ep_c, "air780ep_unregister_urcs")

        self.assert_contains_all(self.air780ep_c, [
            "AIR780EP_CIPRXGET_READY_PREFIX",
            "AIR780EP_CIPRXGET_READY_COMPACT_PREFIX",
            '"+CIPRXGET: 1"',
            '"+CIPRXGET:1"',
            "tcp_readable_handler",
            "tcp_readable_compact_handler",
        ], "air780ep tcp readable URC tokens")
        self.assert_contains_all(register_body, [
            "AIR780EP_CIPRXGET_READY_PREFIX",
            "AIR780EP_CIPRXGET_READY_COMPACT_PREFIX",
            "&self->tcp_readable_handler",
            "&self->tcp_readable_compact_handler",
            "tcp_readable_urc_handler",
        ], "air780ep tcp readable URC registration")
        self.assert_contains_all(unregister_body, [
            "AIR780EP_CIPRXGET_READY_PREFIX",
            "AIR780EP_CIPRXGET_READY_COMPACT_PREFIX",
            "&self->tcp_readable_handler",
            "&self->tcp_readable_compact_handler",
        ], "air780ep tcp readable URC unregistration")

    def test_air780ep_ciprxget_parser_accepts_inline_and_next_line_hex(self):
        body = self.assert_function_body(self.air780ep_c, "find_ciprxget_hex_line")

        self.assert_contains_all(body, [
            "hex_line = cursor;",
            "hex_line = response->lines[i + 1];",
            "hex_len / 2U != (size_t)read_len",
            "hex_nibble(hex_line[j])",
        ], "air780ep ciprxget parser")

    def test_ml307r_tcp_mapping_tokens(self):
        self.assert_contains_all(self.ml307r_c, [
            "AT+MIPCFG=\"cid\",0,%u",
            "AT+MIPCFG=\"encoding\",0,0,1",
            "AT+MIPCFG=\"autofree\",0,0",
            "AT+MIPOPEN=0,\"TCP\",\"%s\",%u,%u,2",
            "AT+MIPSEND=0,%u",
            "at_engine_send_cmd_with_payload",
            "AT+MIPRD=0,%u",
            "+MIPOPEN:",
            "+MIPSEND:",
            "+MIPURC: \"rtcp\"",
            "+MIPURC: \"disconn\"",
            "MODEM_PROTOCOL_TCP",
            ".socket_open = ml307r_socket_open",
            ".socket_send = ml307r_socket_send",
            ".socket_recv = ml307r_socket_recv",
            ".socket_close = ml307r_socket_close",
        ], "ml307r tcp mapping")

    def test_ml307r_mipopen_failure_preserves_modem_error_code(self):
        open_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_open")
        parse_body = self.assert_function_body(self.ml307r_c, "parse_mipopen_response")
        map_body = self.assert_function_body(self.ml307r_c, "ml307r_map_mipopen_result")

        self.assertIn("int *modem_error_code;", self.modem_h)
        self.assert_contains_all(open_body, [
            "uint8_t open_conn_id = 0;",
            "int open_result = 0;",
            "parse_mipopen_response(&ctx.response, &open_conn_id, &open_result)",
            "if (open->modem_error_code)",
            "*open->modem_error_code = open_result;",
            "open_result == 0",
            "ml307r_map_mipopen_result(open_result)",
        ], "ML307R MIPOPEN result handling")
        self.assert_contains_all(parse_body, [
            "find_line_with_prefix(response, \"+MIPOPEN:\")",
            "parse_mqtt_uint_field(&cursor, UINT8_MAX, &parsed_conn_id)",
            "parse_mqtt_uint_field(&cursor, INT_MAX, &parsed_result)",
        ], "ML307R MIPOPEN parser")
        self.assert_contains_all(map_body, [
            "case 558:",
            "return ESP_ERR_TIMEOUT;",
            "case 570:",
            "return ESP_ERR_INVALID_STATE;",
            "return ESP_FAIL;",
        ], "ML307R MIPOPEN result mapper")
        self.assertNotIn('response_contains(&ctx.response, "+MIPOPEN: 0,0")',
                         open_body)

    def test_ml307r_tcp_send_requires_matching_mipsend_ack(self):
        send_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_send")

        self.assertIn("static esp_err_t parse_mipsend_response", self.ml307r_c)
        self.assert_contains_all(send_body, [
            "uint8_t sent_conn_id = 0;",
            "size_t sent_len = 0;",
            "parse_mipsend_response(&ctx.response, &sent_conn_id, &sent_len)",
            "sent_conn_id == ML307R_TCP_CONN_ID",
            "sent_len == send->len",
        ], "ml307r MIPSEND ack validation")
        self.assert_contains_all(send_body, [
            "const at_cmd_options_t options = {",
            ".timeout_ms = send->timeout_ms,",
            ".flags = 0,",
            ".success_matches = NULL,",
            ".success_match_count = 0,",
        ], "ml307r MIPSEND waits for trailing OK")
        self.assertNotIn('response_contains(&ctx.response, "+MIPSEND: 0,")', send_body)
        self.assertNotIn("AT_CMD_FLAG_NO_STANDARD_OK_FINAL", send_body)

    def test_ml307r_tcp_recv_caps_miprd_read_length(self):
        recv_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_recv")

        self.assertIn("ML307R_TCP_MAX_HEX_READ_BYTES", self.ml307r_c)
        self.assert_contains_all(recv_body, [
            "recv->max_len <= UINT_MAX",
            "size_t read_len = recv->max_len;",
            "if (read_len > ML307R_TCP_MAX_HEX_READ_BYTES)",
            "read_len = ML307R_TCP_MAX_HEX_READ_BYTES;",
            "(unsigned int)read_len",
        ], "ml307r capped MIPRD read length")
        self.assertNotIn("(unsigned int)recv->max_len", recv_body)

    def test_ml307r_tcp_recv_ok_without_miprd_line_returns_empty_result(self):
        recv_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_recv")

        self.assert_ordered(recv_body, [
            "ret = ensure_at_ok(&ctx.response, \"AT+MIPRD\");",
            "const char *hex = find_miprd_hex_line(&ctx.response, &remaining_len);",
            "if (!hex) {",
            "find_line_with_prefix(&ctx.response, \"+MIPRD:\")",
            "return ESP_ERR_INVALID_RESPONSE;",
            "result->conn_id = ML307R_TCP_CONN_ID;",
            "result->payload = NULL;",
            "result->payload_len = 0;",
            "result->remaining_len = 0;",
            "result->modem_error_code = 0;",
            "return ESP_OK;",
        ], "ml307r OK-only MIPRD response")
        self.assertNotIn("ESP_RETURN_ON_FALSE(hex", recv_body)

    def test_ml307r_miprd_cap_uses_effective_at_line_buffer(self):
        init_body = self.assert_function_body(self.lwlte_ml307r_c, "lwlte_ml307r_init")
        recv_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_recv")
        cap_body = self.assert_function_body(self.ml307r_c,
                                             "ml307r_tcp_read_len_for_line_buf")

        self.assertIn("int at_rx_line_buf_size;", self.ml307r_h)
        self.assert_ordered(init_body, [
            "int at_rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?",
            "LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE",
            ".rx_line_buf_size = at_rx_line_buf_size,",
            ".at_rx_line_buf_size = at_rx_line_buf_size,",
        ], "ml307r line buffer propagation")
        self.assert_contains_all(cap_body, [
            "ML307R_TCP_MIPRD_LINE_OVERHEAD",
            "rx_line_buf_size",
            "ESP_ERR_INVALID_STATE",
            "*out_read_len = (size_t)line_cap;",
        ], "ml307r line buffer derived cap")
        self.assertRegex(self.ml307r_c,
                         r"#define ML307R_TCP_MIPRD_LINE_OVERHEAD\s+48U")
        self.assert_contains_all(recv_body, [
            "size_t line_cap = 0;",
            "ml307r_tcp_read_len_for_line_buf(self->config.at_rx_line_buf_size, &line_cap)",
            "if (read_len > line_cap)",
            "read_len = line_cap;",
        ], "ml307r recv uses configured line buffer cap")

    def test_ml307r_miprd_parser_rejects_signed_unsigned_fields(self):
        miprd_body = self.assert_function_body(self.ml307r_c, "find_miprd_hex_line")
        uint_parser_start = self.ml307r_c.rindex("static bool parse_mqtt_uint_field")
        uint_parser_body = self.ml307r_c[
            uint_parser_start:
            self.ml307r_c.index("static bool parse_mqtt_comma", uint_parser_start)
        ]

        self.assertIn("parse_mqtt_uint_field(&cursor", miprd_body)
        self.assertNotIn("strtoul(cursor", miprd_body)
        self.assert_ordered(uint_parser_body, [
            "while (isspace((unsigned char)*start))",
            "if (!isdigit((unsigned char)*start))",
            "strtoul(start",
        ], "unsigned parser rejects leading signs")

    def test_ml307r_active_socket_responses_dispatch_tcp_urcs(self):
        helper_body = self.assert_function_body(self.ml307r_c,
                                                "dispatch_tcp_urcs_from_response")

        self.assert_contains_all(helper_body, [
            "ML307R_MIPURC_RTCP_PREFIX",
            "ML307R_MIPURC_DISCONN_PREFIX",
            "bool line_dispatched = false;",
            "tcp_readable_urc_handler(ML307R_MIPURC_RTCP_PREFIX, line, self);",
            "tcp_disconn_urc_handler(ML307R_MIPURC_DISCONN_PREFIX, line, self);",
        ], "ml307r active-response TCP URC dispatch")

        wrapper_body = self.assert_function_body(self.ml307r_c,
                                                 "send_cmd_with_options")
        send_body = self.assert_function_body(self.ml307r_c, "ml307r_socket_send")
        self.assertIn("dispatch_tcp_urcs_from_response(self, &ctx->response);",
                      wrapper_body,
                      "all wrapper-based active responses must scan for TCP URCs")
        self.assertIn("dispatch_tcp_urcs_from_response(self, &ctx.response);",
                      send_body,
                      "payload send bypasses wrapper and must scan TCP URCs")

    def test_tcp_service_does_not_post_data_event_for_zero_length_recv(self):
        core_done_body = self.assert_function_body(self.tcp_c, "handle_core_cmd_done")
        recv_case = core_done_body[core_done_body.index("case CORE_CMD_SOCKET_RECV"):
                                   core_done_body.index("case CORE_CMD_SOCKET_CLOSE")]

        self.assert_ordered(recv_case, [
            "if (recv->payload_len > 0) {",
            "post_tcp_event(conn, LWLTE_TCP_EVENT_DATA, &payload)",
            "if (recv->remaining_len > 0",
        ], "zero-length recv skips DATA event")

    def test_examples_and_docs_are_wired(self):
        self.assert_contains_all(self.example_h + self.example_main + self.example_cmake, [
            "EXAMPLE_AIR780EP_TCP_CLIENT",
            "EXAMPLE_ML307R_TCP_CLIENT",
            "example_air780ep_tcp_client_run",
            "example_ml307r_tcp_client_run",
            '"air780ep_tcp_client.c"',
            '"ml307r_tcp_client.c"',
        ], "example wiring")
        self.assert_contains_all(self.example_kconfig, [
            "EXAMPLE_TCP_HOST",
            "EXAMPLE_TCP_PORT",
            "EXAMPLE_TCP_PAYLOAD",
            "EXAMPLE_TCP_PAYLOAD_HEX",
            "EXAMPLE_TCP_MAX_RX_EVENT_LEN",
        ], "example kconfig")
        for label, text in [("air example", self.air_example), ("ml example", self.ml_example)]:
            self.assert_contains_all(text, [
                "lwlte_tcp_init",
                "esp_event_handler_register(LWLTE_TCP_EVENT",
                "lwlte_tcp_open",
                "lwlte_tcp_send",
                "lwlte_tcp_event_data_release",
                "lwlte_tcp_event_data_release(data);",
                "base.at_engine.rx_line_buf_size = 2048",
            ], label)

    def test_tcp_example_defaults_target_line_echo_server(self):
        self.assert_ordered(self.example_kconfig, [
            "config EXAMPLE_TCP_HOST",
            'default "tcpbin.com"',
            "config EXAMPLE_TCP_PORT",
            "default 4242",
            "config EXAMPLE_TCP_PAYLOAD_HEX",
            'default "68656c6c6f2066726f6d206573702d6c776c7465207463700a"',
        ], "tcp example line echo defaults")
        self.assertIn("TCP Client Service", self.classes_md)
        self.assertIn("tcp_client -> Core -> Modem", self.arch_md)
        self.assertIn("TCP client v1", self.roadmap_md)


if __name__ == "__main__":
    unittest.main()
