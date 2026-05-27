#!/usr/bin/env python3
"""Static regression checks for the Core net_mgr activation flow."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
CORE_PRIV = ROOT / "src/core/core_priv.h"
NET_MGR = ROOT / "src/core/net_mgr.c"
MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP = ROOT / "src/modem/modem_air780ep.c"
CLASSES_DOC = ROOT / "docs/agents/classes.md"


class NetMgrActivationFlowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core_priv = CORE_PRIV.read_text(encoding="utf-8")
        cls.net_mgr = NET_MGR.read_text(encoding="utf-8")
        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv = MODEM_PRIV.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep = AIR780EP.read_text(encoding="utf-8")
        cls.classes_doc = CLASSES_DOC.read_text(encoding="utf-8")

    def test_packet_attach_is_core_visible_modem_operation(self):
        self.assertIn(
            "esp_err_t modem_get_packet_attach_status(modem_t *me, bool *attached);",
            self.modem_h,
        )
        self.assertIn("get_packet_attach_status", self.modem_priv)
        self.assertIn("esp_err_t modem_get_packet_attach_status", self.modem_c)
        self.assertIn("air780ep_get_packet_attach_status", self.air780ep)
        self.assertIn("query_cgatt(self, attached)", self.air780ep)
        self.assertIn("get_packet_attach_status", self.classes_doc)

    def test_net_mgr_has_explicit_wait_stages(self):
        for step_name in [
            "NET_STEP_WAIT_REGISTRATION",
            "NET_STEP_WAIT_PACKET_ATTACH",
            "NET_STEP_QUERY_IP",
        ]:
            self.assertIn(step_name, self.core_priv)
            self.assertIn(step_name, self.net_mgr)

    def test_net_mgr_uses_staged_polling_not_rapid_full_retries(self):
        self.assertIn("NET_MGR_WAIT_POLL_INTERVAL_MS", self.net_mgr)
        self.assertIn("run_activation_loop", self.net_mgr)
        self.assertIn("run_activation_step", self.net_mgr)
        self.assertIn("ESP_ERR_NOT_FINISHED", self.net_mgr)
        self.assertIn("modem_get_packet_attach_status", self.net_mgr)
        self.assertNotIn("while (me->net_mgr.retry_count < me->net_mgr.max_retry)", self.net_mgr)
        self.assertNotIn("activation attempt %d failed", self.net_mgr)

    def test_registration_searching_is_waiting_not_failure(self):
        self.assertIn("registration_denied", self.net_mgr)
        self.assertIn("registration_ready(reg_status)", self.net_mgr)
        self.assertIn("return ESP_ERR_NOT_FINISHED", self.net_mgr)
        self.assertIn("MODEM_REG_DENIED", self.net_mgr)

    def test_online_requires_valid_ip_and_pdp_urc_does_not_bypass_ip(self):
        self.assertIn("pdp.ip_addr[0] == '\\0'", self.net_mgr)
        self.assertIn("pdp->ip_addr[0] == '\\0'", self.net_mgr)
        self.assertIn("old_state != CORE_NET_STATE_ACTIVATING", self.net_mgr)
        self.assertIn("complete_activation", self.net_mgr)


if __name__ == "__main__":
    unittest.main()
