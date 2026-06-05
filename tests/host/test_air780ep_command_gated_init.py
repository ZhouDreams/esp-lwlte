#!/usr/bin/env python3
"""Static regression checks for Air780EP command-gated startup."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
AIR780EP = ROOT / "src/modem/modem_air780ep.c"
CORE_FSM = ROOT / "src/core/core_fsm.c"
LWLTE_H = ROOT / "src/include/lwlte.h"
INIT_FLOW_DOC = ROOT / "docs/modem-init-min-flow.md"
ARCH_DOC = ROOT / "docs/agents/architecture.md"
CLASSES_DOC = ROOT / "docs/agents/classes.md"
AT_DOC = ROOT / "docs/agents/at_cmd_air780ep.md"


def function_body(source: str, signature: str) -> str:
    search_from = 0
    while True:
        start = source.find(signature, search_from)
        if start < 0:
            raise AssertionError(f"missing function definition: {signature}")
        after_signature = start + len(signature)
        brace = source.find("{", after_signature)
        semicolon = source.find(";", after_signature)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        search_from = start + len(signature)

    depth = 0
    for idx in range(brace, len(source)):
        char = source[idx]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:idx]
    raise AssertionError(f"function body not closed for {signature}")


class Air780EpCommandGatedInitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.air780ep = AIR780EP.read_text(encoding="utf-8")
        cls.core_fsm = CORE_FSM.read_text(encoding="utf-8")
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.init_flow_doc = INIT_FLOW_DOC.read_text(encoding="utf-8")
        cls.docs = "\n".join(
            [cls.init_flow_doc]
            + [path.read_text(encoding="utf-8") for path in [ARCH_DOC, CLASSES_DOC, AT_DOC]]
        )

    def test_startup_constants_define_command_probe_policy(self):
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_AT_READY_PROBE_TIMEOUT_MS\s+1000")
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_INIT_RETRY_DELAY_MS\s+500")
        self.assertRegex(self.air780ep, r"#define\s+AIR780EP_INIT_CMD_MAX_ATTEMPTS\s+3")

    def test_rdy_synchronization_path_is_removed(self):
        forbidden_symbols = [
            "AIR780EP_URC_RDY",
            "rdy_handler",
            "rdy_sema",
            "rdy_seen",
            "waiting_rdy",
            "clear_rdy_state",
            "begin_wait_rdy",
            "cancel_wait_rdy",
            "wait_rdy",
            "rdy_urc_handler",
        ]
        for symbol in forbidden_symbols:
            self.assertNotIn(symbol, self.air780ep)

    def test_wait_at_ready_uses_only_at_command(self):
        body = function_body(self.air780ep, "static esp_err_t wait_at_ready(modem_air780ep_t *self)")
        self.assertRegex(
            body,
            r"send_cmd\s*\(\s*self\s*,\s*\"AT\"\s*,\s*&ctx\s*,\s*AIR780EP_AT_READY_PROBE_TIMEOUT_MS\s*\)",
        )
        self.assertIn("AIR780EP_INIT_RETRY_DELAY_MS", body)
        self.assertIn("vTaskDelay", body)
        self.assertIn("ESP_ERR_TIMEOUT", body)
        self.assertNotIn('"ATE0"', body)
        self.assertNotIn("RDY", body)

    def test_basic_init_commands_start_with_ate0_and_retry_with_delay(self):
        body = function_body(self.air780ep, "static esp_err_t run_basic_init_cmds(modem_air780ep_t *self)")
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"', '"AT+CGREG=2"', '"AT+CREG=2"', '"AT*I"']
        last_index = -1
        for token in expected_order:
            index = body.find(token)
            self.assertGreater(index, last_index, token)
            last_index = index
        self.assertIn("AIR780EP_INIT_CMD_MAX_ATTEMPTS", body)
        self.assertIn("AIR780EP_INIT_RETRY_DELAY_MS", body)
        self.assertIn("vTaskDelay", body)

    def test_start_and_reset_sequence_is_command_gated(self):
        for signature in [
            "static esp_err_t air780ep_start(modem_t *me)",
            "static esp_err_t air780ep_reset(modem_t *me)",
        ]:
            body = function_body(self.air780ep, signature)
            for token in [
                "hardware_reset(self)",
                "wait_at_ready(self)",
                "run_basic_init_cmds(self)",
                "ret = register_urcs(self)",
                "finish_modem_ready(me, self)",
                "unregister_urcs(self)",
            ]:
                self.assertIn(token, body, signature)
            unregister_index = body.index("unregister_urcs(self)")
            hardware_reset_index = body.index("hardware_reset(self)")
            wait_at_ready_index = body.index("wait_at_ready(self)")
            run_basic_init_index = body.index("run_basic_init_cmds(self)")
            register_index = body.index("ret = register_urcs(self)")
            finish_ready_index = body.index("finish_modem_ready(me, self)")

            self.assertLess(unregister_index, hardware_reset_index, signature)
            self.assertLess(hardware_reset_index, wait_at_ready_index, signature)
            self.assertLess(wait_at_ready_index, run_basic_init_index, signature)
            self.assertLess(run_basic_init_index, register_index, signature)
            self.assertLess(register_index, finish_ready_index, signature)

            err_cleanup = body[body.index("err:"):]
            self.assertIn("unregister_urcs(self)", err_cleanup, signature)
            self.assertNotIn("cancel_wait_rdy", err_cleanup, signature)
            self.assertNotIn("wait_rdy", body)
            self.assertNotIn("begin_wait_rdy", body)

    def test_core_start_comment_describes_blocking_modem_start_then_network_activation(self):
        handle_start = function_body(self.core_fsm, "static void handle_start(core_t *me)")
        self.assertIn("modem_start", handle_start)
        self.assertIn("AT OK", handle_start)
        self.assertIn("基础 AT", handle_start)
        self.assertIn("net_mgr_start_activation(me)", handle_start)
        self.assertLess(handle_start.index("modem_start(me->modem)"), handle_start.index("net_mgr_start_activation(me)"))

    def test_public_config_comments_use_at_ready_not_rdy_wait(self):
        note_start = self.lwlte_h.index("@note init_ready_timeout_ms")
        field_start = self.lwlte_h.index("uint32_t init_ready_timeout_ms", note_start)
        field_end = self.lwlte_h.index("uint32_t net_activate_timeout_ms", field_start)
        config_timeout_text = self.lwlte_h[note_start:field_end]

        self.assertIn("init_ready_timeout_ms", config_timeout_text)
        self.assertIn("AT OK", config_timeout_text)
        self.assertNotIn("RDY" + " 等待超时", config_timeout_text)
        self.assertNotIn("RDY " + "wait timeout", config_timeout_text)

    def test_docs_describe_at_ok_gate_not_rdy_gate(self):
        for required in [
            "AT OK",
            "AT 通道",
            "命令返回",
            "modem_start()",
            "core_start()",
        ]:
            self.assertIn(required, self.docs)
        forbidden_patterns = [
            r"等待\s*RDY",
            r"等\s*RDY",
            r"wait\s+RDY",
            r"RDY\s+wait",
            r"RDY\s*等待超时",
        ]
        for pattern in forbidden_patterns:
            self.assertIsNone(re.search(pattern, self.docs, re.IGNORECASE), pattern)

    def test_init_flow_doc_uses_cpin_command_polling_not_urc_progress(self):
        for required in [
            "AT+CPIN?",
            "+CPIN:",
            "不释放命令等待",
            "不推进初始化",
            "不推进网络激活",
        ]:
            self.assertIn(required, self.init_flow_doc)

        for forbidden in [
            "或可靠收到等价 `+CPIN: READY` URC",
            "SIM ready 可以消费 `+CPIN: READY`",
            "消费 `+CPIN:`",
            "可消费 `+CPIN:`",
        ]:
            self.assertNotIn(forbidden, self.init_flow_doc)


if __name__ == "__main__":
    unittest.main()
