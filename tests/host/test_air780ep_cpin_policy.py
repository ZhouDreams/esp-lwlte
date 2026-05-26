#!/usr/bin/env python3
"""Static regression checks for the Air780EP AT+CPIN? SIM busy policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src/modem/modem_air780ep.c"
DOC = ROOT / "docs/agents/at_cmd_air780ep.md"


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
        expected_pairs = [
            ("AIR780EP_CME_SIM_NOT_INSERTED", "MODEM_SIM_NOT_INSERTED"),
            ("AIR780EP_CME_SIM_PIN_REQUIRED", "MODEM_SIM_PIN_REQUIRED"),
            ("AIR780EP_CME_SIM_PUK_REQUIRED", "MODEM_SIM_PUK_REQUIRED"),
            ("AIR780EP_CME_SIM_FAILURE", "MODEM_SIM_ERROR"),
            ("AIR780EP_CME_SIM_WRONG", "MODEM_SIM_ERROR"),
        ]

        self.assertIn("static bool sim_status_from_cme_error", self.src)
        for cme_name, status_name in expected_pairs:
            self.assertIn(cme_name, self.src)
            self.assertIn(status_name, self.src)

    def test_air780ep_get_sim_status_retries_only_sim_busy(self):
        self.assertIn("ctx.response.status == AT_RESP_CME_ERROR", self.src)
        self.assertIn("ctx.response.error_code == AIR780EP_CME_SIM_BUSY", self.src)
        self.assertIn("AIR780EP_SIM_READY_POLL_INTERVAL_MS", self.src)
        self.assertIn("vTaskDelay(timeout_ticks(wait_ms))", self.src)
        self.assertIn("return ESP_ERR_TIMEOUT", self.src)

    def test_cpin_documentation_mentions_sim_busy_policy(self):
        self.assertIn("+CME ERROR: 14", self.doc)
        self.assertIn("SIM busy", self.doc)
        self.assertIn("1 秒", self.doc)
        self.assertIn("10 秒", self.doc)


if __name__ == "__main__":
    unittest.main()
