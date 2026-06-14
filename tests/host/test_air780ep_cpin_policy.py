#!/usr/bin/env python3
"""Static regression checks for the Air780EP AT+CPIN? SIM busy policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src/modem/modem_air780ep.c"
DOC = ROOT / "docs/agents/at_cmd_air780ep.md"


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


class Air780EpCpinPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.src = SRC.read_text(encoding="utf-8")
        cls.doc = DOC.read_text(encoding="utf-8")

    def test_busy_polling_constants_are_defined(self):
        self.assertRegex(self.src, r"#define\s+AIR780EP_SIM_READY_TIMEOUT_MS\s+10000")
        self.assertRegex(self.src, r"#define\s+AIR780EP_SIM_READY_POLL_INTERVAL_MS\s+1000")
        self.assertRegex(self.src, r"#define\s+AIR780EP_CME_SIM_BUSY\s+14")

    def test_definite_cme_status_mapping_is_explicit(self):
        body = function_body(
            self.src,
            "static bool sim_status_from_cme_error(int error_code, modem_sim_status_t *status)",
        )
        expected_pairs = [
            ("AIR780EP_CME_SIM_NOT_INSERTED", "MODEM_SIM_NOT_INSERTED"),
            ("AIR780EP_CME_SIM_PIN_REQUIRED", "MODEM_SIM_PIN_REQUIRED"),
            ("AIR780EP_CME_SIM_PUK_REQUIRED", "MODEM_SIM_PUK_REQUIRED"),
            ("AIR780EP_CME_SIM_FAILURE", "MODEM_SIM_ERROR"),
            ("AIR780EP_CME_SIM_WRONG", "MODEM_SIM_ERROR"),
        ]

        for cme_name, status_name in expected_pairs:
            self.assertIn(cme_name, body)
            self.assertIn(status_name, body)

    def test_air780ep_get_sim_status_retries_sim_busy_by_timer_only(self):
        body = function_body(self.src, "static esp_err_t air780ep_get_sim_status(modem_handle_t *me, modem_sim_status_t *status)")

        self.assertIn("ctx.response.status == AT_RESP_CME_ERROR", body)
        self.assertIn("ctx.response.error_code == AIR780EP_CME_SIM_BUSY", body)
        self.assertIn("AIR780EP_SIM_READY_POLL_INTERVAL_MS", body)
        self.assertIn("vTaskDelay(timeout_ticks(wait_ms))", body)
        self.assertIn("return ESP_ERR_TIMEOUT", body)
        self.assertNotIn("cpin_ready_sema", body)
        self.assertNotIn("waiting_cpin_ready", body)
        self.assertNotIn("wait URC or", body)
        self.assertNotIn("+CPIN: READY URC received", body)

    def test_cpin_documentation_mentions_sim_busy_policy(self):
        self.assertIn("+CME ERROR: 14", self.doc)
        self.assertIn("SIM busy", self.doc)
        self.assertIn("1 秒", self.doc)
        self.assertIn("10 秒", self.doc)


if __name__ == "__main__":
    unittest.main()
