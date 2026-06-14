#!/usr/bin/env python3
"""Static regression checks for the ML307R modem implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
LWLTE_H = ROOT / "src/include/lwlte.h"
SRC_CMAKE = ROOT / "src/CMakeLists.txt"
ML307R_H = ROOT / "src/modem/modem_ml307r.h"
ML307R_C = ROOT / "src/modem/modem_ml307r.c"
LWLTE_ML307R_C = ROOT / "src/lwlte/lwlte_ml307r.c"


def read_optional(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


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


class Ml307rContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional(LWLTE_H)
        cls.src_cmake = read_optional(SRC_CMAKE)
        cls.ml307r_h = read_optional(ML307R_H)
        cls.ml307r_c = read_optional(ML307R_C)
        cls.lwlte_ml307r_c = read_optional(LWLTE_ML307R_C)

    def test_public_api_and_build_entries_exist(self):
        self.assertTrue(ML307R_H.exists(), "missing modem_ml307r.h")
        self.assertTrue(ML307R_C.exists(), "missing modem_ml307r.c")
        self.assertTrue(LWLTE_ML307R_C.exists(), "missing lwlte_ml307r.c")
        for token in [
            "lwlte_mqtt_config_t",
            "lwlte_ml307r_config_t",
            "esp_err_t lwlte_mqtt_init(lwlte_handle_t *me, const lwlte_mqtt_config_t *config);",
            "esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,",
        ]:
            self.assertIn(token, self.lwlte_h)
        for token in [
            '"modem/modem_ml307r.c"',
            '"lwlte/lwlte_ml307r.c"',
        ]:
            self.assertIn(token, self.src_cmake)

    def test_modem_header_declares_config_and_factory(self):
        for token in [
            "modem_ml307r_config_t",
            "gpio_num_t en_pin;",
            "uint32_t reset_pulse_ms;",
            "uint32_t ready_timeout_ms;",
            "uint32_t default_cmd_timeout_ms;",
            "modem_handle_t *modem_ml307r_create(at_engine_handle_t *at,",
        ]:
            self.assertIn(token, self.ml307r_h)

    def test_startup_uses_at_probe_not_matready(self):
        for token in [
            "#define ML307R_AT_READY_PROBE_TIMEOUT_MS 1000",
            "#define ML307R_INIT_RETRY_DELAY_MS",
            "static esp_err_t wait_at_ready(modem_ml307r_t *self)",
            "static esp_err_t run_basic_init_cmds(modem_ml307r_t *self)",
        ]:
            self.assertIn(token, self.ml307r_c)
        wait_body = function_body(self.ml307r_c, "static esp_err_t wait_at_ready(modem_ml307r_t *self)")
        self.assertRegex(wait_body, r"send_cmd\s*\(\s*self\s*,\s*\"AT\"")
        self.assertIn("ML307R_AT_READY_PROBE_TIMEOUT_MS", wait_body)
        self.assertIn("ESP_ERR_TIMEOUT", wait_body)
        self.assertNotIn("MATREADY", wait_body)
        self.assertNotIn("+MATREADY", self.ml307r_c)

    def test_basic_init_commands_and_start_reset_order(self):
        init_body = function_body(self.ml307r_c, "static esp_err_t run_basic_init_cmds(modem_ml307r_t *self)")
        expected_order = ['"ATE0"', '"AT+CMEE=1"', '"AT+CEREG=2"', '"AT+CGREG=2"', '"AT+CREG=2"']
        last = -1
        for token in expected_order:
            index = init_body.find(token)
            self.assertGreater(index, last, token)
            last = index
        for signature in [
            "static esp_err_t ml307r_start(modem_handle_t *me)",
            "static esp_err_t ml307r_reset(modem_handle_t *me)",
        ]:
            body = function_body(self.ml307r_c, signature)
            for token in [
                "hardware_reset(self)",
                "wait_at_ready(self)",
                "run_basic_init_cmds(self)",
                "ret = register_urcs(self)",
                "finish_modem_ready(me, self)",
            ]:
                self.assertIn(token, body, signature)
            self.assertLess(body.index("hardware_reset(self)"), body.index("wait_at_ready(self)"))
            self.assertLess(body.index("wait_at_ready(self)"), body.index("run_basic_init_cmds(self)"))
            self.assertLess(body.index("run_basic_init_cmds(self)"), body.index("ret = register_urcs(self)"))

    def test_facade_factory_mirrors_air780ep_lifecycle(self):
        for token in [
            '#include "modem_ml307r.h"',
            "esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,",
            "at_engine_create(&at_config)",
            "modem_ml307r_create(me->at, &modem_config)",
            "core_create(&core_config, me->modem)",
            "facade_ready_handler, me",
            "ping_client_create(me->core)",
        ]:
            self.assertIn(token, self.lwlte_ml307r_c)
        self.assertNotIn("core_start", self.lwlte_ml307r_c)
        self.assertNotIn("modem_start", self.lwlte_ml307r_c)

    def test_facade_defaults_ml307r_line_buffer_for_direct_mqtt_urcs(self):
        self.assertIn("#define LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE 2048", self.lwlte_ml307r_c)
        init_body = function_body(
            self.lwlte_ml307r_c,
            "esp_err_t lwlte_ml307r_init(const lwlte_ml307r_config_t *config,",
        )
        self.assertIn(
            ".rx_line_buf_size = config->at_rx_line_buf_size ?",
            init_body,
        )
        self.assertIn(
            "LWLTE_ML307R_DEFAULT_AT_LINE_BUF_SIZE",
            init_body,
        )

    def test_identity_status_and_registration_mapping_exists(self):
        for token in [
            "AT+CGSN",
            "AT+CIMI",
            "AT+MCCID",
            "AT+CGMM",
            "AT+CGMR",
            "AT+CPIN?",
            "AT+CSQ",
            "AT+CEREG?",
            "AT+CGREG?",
            "AT+CREG?",
            "AT+CGATT?",
            "parse_sim_status_line",
            "map_reg_status",
            "rssi_dbm_valid",
            ".get_info = ml307r_get_info",
            ".get_sim_status = ml307r_get_sim_status",
            ".get_signal = ml307r_get_signal",
            ".get_registration = ml307r_get_registration",
            ".get_packet_attach_status = ml307r_get_packet_attach_status",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_mipcall_network_mapping_exists(self):
        for token in [
            "ML307R_URC_MIPCALL",
            "AT+CGDCONT=%u,\"IPV4V6\",\"%s\"",
            "AT+MIPCALL?",
            "AT+MIPCALL=1,%u",
            "AT+MIPCALL=0,%u",
            "parse_mipcall_line",
            "query_mipcall",
            ".activate_pdp = ml307r_activate_pdp",
            ".deactivate_pdp = ml307r_deactivate_pdp",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_mqtt_command_mapping_exists(self):
        for token in [
            "ML307R_URC_MQTTURC",
            "#define ML307R_MQTT_MAX_PAYLOAD_LEN      1024U",
            "AT+MQTTCFG=\"version\",0,4",
            "AT+MQTTCFG=\"cid\",0,1",
            "AT+MQTTCFG=\"keepalive\",0,%u",
            "AT+MQTTCFG=\"clean\",0,%u",
            "AT+MQTTCFG=\"encoding\",0,1,0",
            "AT+MQTTCFG=\"cached\",0,0",
            "AT+MQTTCONN=0,\"%s\",%u,\"%s\",\"%s\",\"%s\"",
            "AT+MQTTDISC=0",
            "AT+MQTTSUB=0,\"%s\",%u",
            "AT+MQTTUNSUB=0,\"%s\"",
            "AT+MQTTPUB=0,\\\"%s\\\",%u,%u,%u,%u,\\\"%s\\\"",
            "hex_encode_payload",
            "parse_mqtt_conn_urc",
            "parse_mqtt_publish_urc",
            "handle_mqtturc",
        ]:
            self.assertIn(token, self.ml307r_c)

        configure_body = function_body(
            self.ml307r_c,
            "static esp_err_t ml307r_mqtt_configure(modem_handle_t *me,",
        )
        self.assertNotIn("escape_at_string", configure_body)
        self.assertNotIn("char *host", configure_body)

        publish_body = function_body(
            self.ml307r_c,
            "static esp_err_t ml307r_mqtt_publish(modem_handle_t *me,",
        )
        self.assertIn("hex_payload = hex_encode_payload", publish_body)
        self.assertIn("(unsigned int)publish->payload_len, hex_payload", publish_body)
        self.assertNotIn("at_text_payload_safe", publish_body)
        self.assertNotIn("copy_payload_text", publish_body)

        hex_body = function_body(
            self.ml307r_c,
            "static char *hex_encode_payload(const uint8_t *payload, size_t payload_len)",
        )
        self.assertIn('static const char hex[] = "0123456789ABCDEF";', hex_body)
        self.assertIn("payload_len > ML307R_MQTT_MAX_PAYLOAD_LEN", hex_body)
        self.assertIn("payload_len > (SIZE_MAX - 1U) / 2U", hex_body)
        self.assertIn("text[i * 2U] = hex[payload[i] >> 4];", hex_body)
        self.assertIn("text[(i * 2U) + 1U] = hex[payload[i] & 0x0FU];", hex_body)
        self.assertIn("text[payload_len * 2U] = '\\0';", hex_body)

        handle_body = function_body(
            self.ml307r_c,
            "static void handle_mqtturc(modem_ml307r_t *self, const char *line)",
        )
        self.assertRegex(
            handle_body,
            r"if\s*\(ret\s*==\s*ESP_ERR_INVALID_RESPONSE\)\s*\{\s*"
            r"ESP_LOGW\(TAG, \"parse MQTT conn URC failed:",
        )
        self.assertIn("ret != ESP_OK && ret != ESP_FAIL", handle_body)

    def test_ping_mapping_exists(self):
        for token in [
            "ML307R_MPING_PREFIX",
            "AT+MPING=\"%s\",%u,%u,%u,1",
            "parse_mping_reply_line",
            "parse_mping_statistics_line",
            "calculate_ping_summary",
            ".ping = ml307r_ping",
        ]:
            self.assertIn(token, self.ml307r_c)

    def test_urc_registration_and_callback_constraints(self):
        register_body = function_body(self.ml307r_c, "static esp_err_t register_urcs(modem_ml307r_t *self)")
        for token in [
            "ML307R_URC_CPIN",
            "ML307R_URC_CREG",
            "ML307R_URC_CEREG",
            "ML307R_URC_CGREG",
            "ML307R_URC_MIPCALL",
            "ML307R_URC_MQTTURC",
            "at_engine_register_urc",
        ]:
            self.assertIn(token, register_body)
        for handler in [
            "static void cpin_urc_handler",
            "static void reg_urc_handler",
            "static void mipcall_urc_handler",
            "static void mqtturc_urc_handler",
        ]:
            body = function_body(self.ml307r_c, handler)
            self.assertNotIn("send_cmd(", body)
            self.assertNotIn("at_engine_send_cmd", body)


if __name__ == "__main__":
    unittest.main()
