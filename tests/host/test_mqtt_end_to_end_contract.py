#!/usr/bin/env python3
"""Static regression checks for the MQTT end-to-end implementation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]

LWLTE_H = ROOT / "src/include/lwlte.h"
AIR780EP_H = ROOT / "src/include/lwlte_air780ep.h"
LWLTE_PRIV = ROOT / "src/lwlte/lwlte_priv.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"

AT_ENGINE_H = ROOT / "src/at_engine/at_engine.h"
AT_ENGINE_C = ROOT / "src/at_engine/at_engine.c"

MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP_C = ROOT / "src/modem/modem_air780ep.c"

CORE_H = ROOT / "src/core/core.h"
CORE_PRIV = ROOT / "src/core/core_priv.h"
CORE_C = ROOT / "src/core/core.c"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"

MQTT_H = ROOT / "src/mqtt_client/mqtt_client.h"
MQTT_PRIV = ROOT / "src/mqtt_client/mqtt_client_priv.h"
MQTT_C = ROOT / "src/mqtt_client/mqtt_client.c"
SRC_CMAKE = ROOT / "src/CMakeLists.txt"


class MqttEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.air780ep_h = AIR780EP_H.read_text(encoding="utf-8")
        cls.lwlte_priv = LWLTE_PRIV.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")
        cls.lwlte_air780ep_c = LWLTE_AIR780EP_C.read_text(encoding="utf-8")

        cls.at_engine_h = AT_ENGINE_H.read_text(encoding="utf-8")
        cls.at_engine_c = AT_ENGINE_C.read_text(encoding="utf-8")

        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv = MODEM_PRIV.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep_c = AIR780EP_C.read_text(encoding="utf-8")

        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_priv = CORE_PRIV.read_text(encoding="utf-8")
        cls.core_c = CORE_C.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")

        cls.mqtt_h = MQTT_H.read_text(encoding="utf-8")
        cls.mqtt_priv = MQTT_PRIV.read_text(encoding="utf-8")
        cls.mqtt_c = MQTT_C.read_text(encoding="utf-8")
        cls.src_cmake = SRC_CMAKE.read_text(encoding="utf-8")

    def test_public_api_and_air780ep_mqtt_config_exist(self):
        for token in [
            "typedef enum {",
            "LWLTE_MQTT_STATE_STOPPED",
            "LWLTE_MQTT_STATE_CONNECTED",
            "lwlte_mqtt_state_t",
            "lwlte_mqtt_msg_t",
            "LWLTE_EVENT_MQTT_CONNECTED",
            "LWLTE_EVENT_MQTT_DATA",
            "esp_err_t lwlte_mqtt_start(lwlte_t *me);",
            "esp_err_t lwlte_mqtt_stop(lwlte_t *me);",
            "esp_err_t lwlte_mqtt_get_state(lwlte_t *me, lwlte_mqtt_state_t *state);",
            "esp_err_t lwlte_mqtt_subscribe(lwlte_t *me, const char *topic, uint8_t qos);",
            "esp_err_t lwlte_mqtt_unsubscribe(lwlte_t *me, const char *topic);",
            "esp_err_t lwlte_mqtt_publish(lwlte_t *me, const char *topic,",
        ]:
            self.assertIn(token, self.lwlte_h)

        for token in [
            "lwlte_air780ep_config_mqtt_client_t",
            "bool enabled;",
            "const char *host;",
            "uint16_t port;",
            "const char *client_id;",
            "lwlte_air780ep_config_mqtt_client_t mqtt_client;",
        ]:
            self.assertIn(token, self.air780ep_h)

    def test_mqtt_service_layer_exists_and_does_not_cross_boundaries(self):
        self.assertIn('"mqtt_client/mqtt_client.c"', self.src_cmake)
        self.assertIn("PRIV_INCLUDE_DIRS lwlte core mqtt_client modem at_engine", self.src_cmake)
        self.assertIn("typedef struct mqtt_client mqtt_client_t;", self.mqtt_h)
        self.assertIn("mqtt_client_create", self.mqtt_h)
        self.assertIn("core_submit_cmd", self.mqtt_c)
        self.assertIn("esp_event_handler_register_with", self.mqtt_c)
        self.assertIn("MQTT_SIG_CORE_CMD_DONE", self.mqtt_priv)
        self.assertIn("MQTT_SIG_PROTOCOL_DATA", self.mqtt_priv)

        forbidden_includes = [
            '#include "modem.h"',
            '#include "modem_air780ep.h"',
            '#include "at_engine.h"',
            '#include "core_priv.h"',
        ]
        for include in forbidden_includes:
            self.assertNotIn(include, self.mqtt_c)
            self.assertNotIn(include, self.mqtt_h)
            self.assertNotIn(include, self.mqtt_priv)

    def test_core_command_queue_contract_exists(self):
        for token in [
            "CORE_EVENT_PROTOCOL_DATA",
            "CORE_EVENT_PROTOCOL_CLOSED",
            "CORE_PROTOCOL_MQTT",
            "core_protocol_data_t",
            "CORE_CMD_MQTT_CONFIG",
            "CORE_CMD_MQTT_OPEN",
            "CORE_CMD_MQTT_LOGIN",
            "CORE_CMD_MQTT_DISCONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "core_cmd_t",
            "core_submit_cmd(core_t *me, const core_cmd_t *cmd);",
        ]:
            self.assertIn(token, self.core_h)

        self.assertIn("CORE_SIG_SERVICE_CMD", self.core_priv)
        self.assertIn("core_cmd_t *service_cmd;", self.core_priv)
        self.assertIn("static core_cmd_t *clone_core_cmd", self.core_c)
        self.assertIn("static void free_core_cmd", self.core_c)
        self.assertIn("esp_err_t core_submit_cmd", self.core_c)
        self.assertIn("handle_service_cmd", self.core_fsm_c)
        self.assertIn("modem_mqtt_config", self.core_fsm_c)
        self.assertIn("modem_mqtt_publish", self.core_fsm_c)

    def test_modem_mqtt_ops_and_air780ep_commands_exist(self):
        for token in [
            "modem_mqtt_config_t",
            "modem_mqtt_open_t",
            "modem_mqtt_login_t",
            "modem_mqtt_topic_t",
            "modem_mqtt_publish_t",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "MODEM_PROTOCOL_MQTT",
            "modem_mqtt_config(modem_t *me",
            "modem_mqtt_publish(modem_t *me",
        ]:
            self.assertIn(token, self.modem_h)

        for token in [
            "mqtt_config",
            "mqtt_open",
            "mqtt_login",
            "mqtt_disconnect",
            "mqtt_subscribe",
            "mqtt_unsubscribe",
            "mqtt_publish",
        ]:
            self.assertIn(token, self.modem_priv)

        for token in [
            "esp_err_t modem_mqtt_config",
            "esp_err_t modem_mqtt_publish",
            "release_event_payload",
        ]:
            self.assertIn(token, self.modem_c)

        for token in [
            "AIR780EP_URC_MSUB",
            "AT+MCONFIG",
            "AT+MIPSTART",
            "AT+MCONNECT",
            "AT+MDISCONNECT",
            "AT+MSUB",
            "AT+MUNSUB",
            "AT+MPUBEX",
            "air780ep_mqtt_config",
            "air780ep_mqtt_publish",
            "handle_msub_urc",
        ]:
            self.assertIn(token, self.air780ep_c)

    def test_at_engine_payload_prompt_support_exists(self):
        self.assertIn("at_engine_send_cmd_with_payload", self.at_engine_h)
        self.assertIn("const uint8_t *payload", self.at_engine_h)
        self.assertIn("payload_prompt", self.at_engine_c)
        self.assertIn("write_payload", self.at_engine_c)
        self.assertIn("uart_write_bytes", self.at_engine_c)

    def test_protocol_data_path_symbols_exist(self):
        self.assertIn("MODEM_EVENT_PROTOCOL_DATA", self.air780ep_c)
        self.assertIn("clone_protocol_data", self.core_c)
        self.assertIn("CORE_EVENT_PROTOCOL_DATA", self.core_fsm_c)
        self.assertIn("handle_core_event", self.mqtt_c)
        self.assertIn("MQTT_CLIENT_EVENT_DATA", self.mqtt_c)
        self.assertIn("LWLTE_EVENT_MQTT_DATA", self.lwlte_c)
        self.assertIn("lwlte_handle_mqtt_event", self.lwlte_priv)
        self.assertIn("mqtt_client_register_event_callback", self.lwlte_air780ep_c)


if __name__ == "__main__":
    unittest.main()
