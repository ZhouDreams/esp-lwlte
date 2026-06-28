#!/usr/bin/env python3
"""Static regression checks for the MQTT end-to-end implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]

LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_AIR780EP_H = ROOT / "src/include/lwlte_air780ep.h"
LWLTE_PRIV = ROOT / "src/lwlte/lwlte_priv.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"
CLASSES_MD = ROOT / "docs/agents/classes.md"

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


def read_optional(path: Path) -> str:
    """Read a source file, returning empty text for planned future files."""
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class MqttEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read_optional(LWLTE_H)
        cls.lwlte_priv = read_optional(LWLTE_PRIV)
        cls.lwlte_c = read_optional(LWLTE_C)
        cls.lwlte_air780ep_c = read_optional(LWLTE_AIR780EP_C)
        cls.classes_md = read_optional(CLASSES_MD)

        cls.at_engine_h = read_optional(AT_ENGINE_H)
        cls.at_engine_c = read_optional(AT_ENGINE_C)

        cls.modem_h = read_optional(MODEM_H)
        cls.modem_priv = read_optional(MODEM_PRIV)
        cls.modem_c = read_optional(MODEM_C)
        cls.air780ep_c = read_optional(AIR780EP_C)

        cls.core_h = read_optional(CORE_H)
        cls.core_priv = read_optional(CORE_PRIV)
        cls.core_c = read_optional(CORE_C)
        cls.core_fsm_c = read_optional(CORE_FSM_C)

        cls.mqtt_h = read_optional(MQTT_H)
        cls.mqtt_priv = read_optional(MQTT_PRIV)
        cls.mqtt_c = read_optional(MQTT_C)
        cls.src_cmake = read_optional(SRC_CMAKE)

    def test_public_api_and_air780ep_mqtt_config_exist(self):
        for token in [
            "typedef enum {",
            "LWLTE_MQTT_STATE_STOPPED",
            "LWLTE_MQTT_STATE_CONNECTED",
            "lwlte_mqtt_state_t",
            "lwlte_mqtt_msg_t",
            "LWLTE_MQTT_EVENT_CONNECTED",
            "LWLTE_MQTT_EVENT_DATA",
            "esp_err_t lwlte_mqtt_start(lwlte_handle_t me);",
            "esp_err_t lwlte_mqtt_stop(lwlte_handle_t me);",
            "esp_err_t lwlte_mqtt_get_state(lwlte_handle_t me, lwlte_mqtt_state_t *state);",
            "esp_err_t lwlte_mqtt_subscribe(lwlte_handle_t me, const char *topic, uint8_t qos);",
            "esp_err_t lwlte_mqtt_unsubscribe(lwlte_handle_t me, const char *topic);",
            "esp_err_t lwlte_mqtt_publish(lwlte_handle_t me, const char *topic,",
        ]:
            self.assertIn(token, self.lwlte_h)

        for token in [
            "lwlte_mqtt_config_t",
            "const char *host;",
            "uint16_t port;",
            "const char *client_id;",
            "esp_err_t lwlte_mqtt_init(lwlte_handle_t me, const lwlte_mqtt_config_t *config);",
            "esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,",
        ]:
            self.assertIn(token, self.lwlte_h)

    def test_lwlte_h_is_the_only_public_lwlte_header(self):
        self.assertFalse(
            LWLTE_AIR780EP_H.exists(),
            "Air780EP public declarations must live in lwlte.h",
        )
        self.assertIn('#include "driver/gpio.h"', self.lwlte_h)
        self.assertIn('#include "driver/uart.h"', self.lwlte_h)
        self.assertIn("esp_err_t lwlte_air780ep_init", self.lwlte_h)
        self.assertIn("esp_err_t lwlte_destroy", self.lwlte_h)
        self.assertLess(
            self.lwlte_h.index("esp_err_t lwlte_air780ep_init"),
            self.lwlte_h.index("esp_err_t lwlte_destroy"),
        )
        self.assertIn('#include "lwlte.h"', self.lwlte_air780ep_c)
        self.assertNotIn('#include "lwlte_air780ep.h"', self.lwlte_air780ep_c)

    def test_mqtt_service_layer_exists_and_does_not_cross_boundaries(self):
        self.assertIn('"mqtt_client/mqtt_client.c"', self.src_cmake)
        match = re.search(r"PRIV_INCLUDE_DIRS(?P<body>.*?)(?:\n\s*[A-Z_]+|\))", self.src_cmake, re.DOTALL)
        self.assertIsNotNone(match, "missing PRIV_INCLUDE_DIRS")
        for include_dir in ["lwlte", "core", "mqtt_client", "modem", "at_engine"]:
            self.assertRegex(match.group("body"), rf"\b{include_dir}\b")
        self.assertIn("typedef struct mqtt_client_t *mqtt_client_handle_t;", self.mqtt_h)
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

    def test_mqtt_service_owns_signal_payloads_and_callback_lifecycle(self):
        for token in [
            "typedef struct mqtt_protocol_data_owned",
            "static void free_mqtt_fsm_sig_payload",
            "mqtt_client_destroy",
            "mqtt_fsm_task",
            "me->fsm_task == xTaskGetCurrentTaskHandle()",
            "core_register_protocol_callback(core, CORE_PROTOCOL_MQTT",
            "core_register_protocol_closed_callback(core, CORE_PROTOCOL_MQTT",
            "LWLTE_MQTT_EVENT",
            "drain_fsm_queue_payloads",
        ]:
            self.assertIn(token, self.mqtt_c + self.mqtt_priv)

        for token in [
            "clone_string",
            "clone_payload",
            "xQueueSend(me->fsm_queue, &sig, 0)",
            "ESP_ERR_TIMEOUT",
        ]:
            self.assertIn(token, self.mqtt_c)

        for old_token in [
            "event_callback_active",
            "event_callback_task",
            "event_callback_done_sema",
            "event_callback_waiting",
            "wait_event_callbacks_idle",
            "MQTT_CLIENT_EVENT_DATA",
            "mqtt_client_register_event_callback",
            "core_release_event_payload",
        ]:
            self.assertNotIn(old_token, self.mqtt_c + self.mqtt_priv)

    def test_mqtt_service_submits_core_commands_without_holding_lock(self):
        self.assertIn("static esp_err_t submit_core_cmd", self.mqtt_c)
        submit_start = self.mqtt_c.rindex("static esp_err_t submit_core_cmd")
        submit_body = self.mqtt_c[
            submit_start:
            self.mqtt_c.index("static esp_err_t begin_connect", submit_start)
        ]
        self.assertIn("core_submit_cmd(me->core, &cmd);", submit_body)
        before_submit = submit_body[:submit_body.index("core_submit_cmd(me->core, &cmd);")]
        self.assertNotIn("xSemaphoreTake(me->lock", before_submit)
        self.assertNotIn("xSemaphoreGive(me->lock", before_submit)

        fsm_body = self.mqtt_c[
            self.mqtt_c.index("static esp_err_t begin_connect"):
            self.mqtt_c.index("mqtt_client_handle_t mqtt_client_create")
        ]
        for token in [
            "submit_core_cmd(me, CORE_CMD_MQTT_CONFIGURE",
            "submit_core_cmd(me, CORE_CMD_MQTT_TCP_CONNECT",
            "submit_core_cmd(me, CORE_CMD_MQTT_CONNECT",
            "submit_core_cmd(me, CORE_CMD_MQTT_SUBSCRIBE",
            "submit_core_cmd(me, CORE_CMD_MQTT_UNSUBSCRIBE",
            "submit_core_cmd(me, CORE_CMD_MQTT_PUBLISH",
            "submit_core_cmd(me, CORE_CMD_MQTT_DISCONNECT",
        ]:
            self.assertIn(token, fsm_body)

    def test_mqtt_stop_records_request_and_waits_for_disconnect_completion(self):
        for token in [
            "bool stop_requested;",
            "bool transport_open;",
            "static void complete_stop",
            "static void request_stop_disconnect",
            "mqtt_stop_step_t",
            "MQTT_STOP_STEP_DISCONNECT",
            "MQTT_STOP_STEP_TCP_DISCONNECT",
            "stop_step",
            "me->stop_requested = true;",
            "if (me->pending_cmd.active) {",
            "return;",
            "submit_core_cmd(me, CORE_CMD_MQTT_DISCONNECT",
            "MQTT_CLIENT_OPERATION_DISCONNECT",
            "complete_stop(me);",
        ]:
            self.assertIn(token, self.mqtt_c + self.mqtt_priv)

        stop_start = self.mqtt_c.rindex("static void handle_stop")
        stop_body = self.mqtt_c[
            stop_start:
            self.mqtt_c.index("static void complete_stop", stop_start)
        ]
        self.assertIn("me->stop_requested = true;", stop_body)
        self.assertIn("if (me->pending_cmd.active) {", stop_body)
        self.assertLess(
            stop_body.index("if (me->pending_cmd.active) {"),
            stop_body.index("request_stop_disconnect(me);")
        )
        self.assertNotIn("me->pending_cmd.active = false;", stop_body)
        self.assertNotIn("post_mqtt_event(me, MQTT_CLIENT_EVENT_STOPPED", stop_body)

    def test_mqtt_stop_defers_runtime_events_and_completes_on_disconnect_or_close(self):
        self.assertIn("CORE_CMD_MQTT_TCP_DISCONNECT", self.mqtt_c)
        self.assertIn("submit_core_cmd(me, CORE_CMD_MQTT_TCP_DISCONNECT", self.mqtt_c)

        stop_disconnect_start = self.mqtt_c.rindex("static void request_stop_disconnect")
        stop_disconnect_body = self.mqtt_c[
            stop_disconnect_start:
            self.mqtt_c.index("static void handle_core_cmd_done", stop_disconnect_start)
        ]
        self.assertIn("CORE_CMD_MQTT_DISCONNECT", stop_disconnect_body)
        self.assertIn("submit_core_cmd(me, CORE_CMD_MQTT_DISCONNECT", stop_disconnect_body)
        self.assertIn("CORE_CMD_MQTT_TCP_DISCONNECT", stop_disconnect_body)
        self.assertIn("submit_core_cmd(me, CORE_CMD_MQTT_TCP_DISCONNECT", stop_disconnect_body)

        done_start = self.mqtt_c.rindex("static void handle_core_cmd_done")
        done_body = self.mqtt_c[
            done_start:
            self.mqtt_c.index("static void handle_runtime_operation", done_start)
        ]
        for token in [
            "if (me->stop_requested) {",
            "sig->core_cmd_type == CORE_CMD_MQTT_CONNECT",
            "sig->core_cmd_type == CORE_CMD_MQTT_DISCONNECT",
            "sig->core_result == CORE_CMD_RESULT_OK",
            "sig->core_cmd_type == CORE_CMD_MQTT_TCP_DISCONNECT",
            "set_state(me, MQTT_CLIENT_STATE_CONNECTED);",
            "complete_stop(me);",
            "request_stop_disconnect(me);",
            "return;",
        ]:
            self.assertIn(token, done_body)

        self.assertLess(
            done_body.index("if (me->stop_requested) {"),
            done_body.index("if (sig->core_result != CORE_CMD_RESULT_OK)")
        )
        self.assertLess(
            done_body.index("if (me->stop_requested) {"),
            done_body.index("LWLTE_MQTT_EVENT_SUBSCRIBED")
        )
        stop_requested_body = done_body[
            done_body.index("if (me->stop_requested) {"):
            done_body.index("if (sig->core_result != CORE_CMD_RESULT_OK)")
        ]
        self.assertLess(
            stop_requested_body.index("CORE_CMD_MQTT_DISCONNECT"),
            stop_requested_body.index("CORE_CMD_MQTT_TCP_DISCONNECT")
        )
        self.assertLess(
            stop_requested_body.index("set_state(me, MQTT_CLIENT_STATE_CONNECTED);"),
            stop_requested_body.index("request_stop_disconnect(me);")
        )

        signal_start = self.mqtt_c.rindex("static void handle_signal")
        signal_body = self.mqtt_c[
            signal_start:
            self.mqtt_c.index("static void handle_start", signal_start)
        ]
        self.assertIn("if (me->stop_requested) {", signal_body)
        self.assertIn("complete_stop(me);", signal_body)
        self.assertLess(
            signal_body.index("if (me->stop_requested) {"),
            signal_body.index("LWLTE_MQTT_EVENT_DISCONNECTED")
        )

    def test_mqtt_data_event_uses_event_bus_with_owned_payload(self):
        """MQTT events are posted via esp_event to LWLTE_MQTT_EVENT base."""
        post_start = self.mqtt_c.rindex("static esp_err_t post_mqtt_event")
        post_body = self.mqtt_c[
            post_start:
            self.mqtt_c.index("static void post_error_event", post_start)
        ]
        self.assertIn("esp_event_post_to", post_body)
        self.assertIn("esp_event_post(", post_body)
        self.assertIn("LWLTE_MQTT_EVENT", post_body)

        for token in [
            "LWLTE_MQTT_EVENT_DATA payloads carry heap-owned topic/payload that must be released",
            "lwlte_mqtt_event_data_release",
        ]:
            self.assertIn(token, self.classes_md)

    def test_core_command_queue_contract_exists(self):
        for token in [
            "CORE_PROTOCOL_MQTT",
            "core_protocol_data_t",
            "core_protocol_callback_t",
            "core_register_protocol_callback",
            "core_register_protocol_closed_callback",
            "CORE_CMD_MQTT_CONFIGURE",
            "CORE_CMD_MQTT_TCP_CONNECT",
            "CORE_CMD_MQTT_CONNECT",
            "CORE_CMD_MQTT_DISCONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "core_cmd_t",
            "core_submit_cmd(core_handle_t me, const core_cmd_t *cmd);",
        ]:
            self.assertIn(token, self.core_h)

        old_tokens = [
            "CORE_CMD_MQTT_" + "CONFIG,",
            "CORE_CMD_MQTT_" + "CONFIG =",
            "CORE_CMD_MQTT_" + "OPEN",
            "CORE_CMD_MQTT_" + "LOGIN",
            "mqtt_" + "open",
            "mqtt_" + "login",
        ]
        for token in old_tokens:
            self.assertNotIn(token, self.core_h)

        self.assertIn("CORE_SIG_SERVICE_CMD", self.core_priv)
        self.assertIn("core_cmd_t *service_cmd;", self.core_priv)
        self.assertIn("static core_cmd_t *clone_core_cmd", self.core_c)
        self.assertIn("static void free_core_cmd", self.core_c)
        self.assertIn("esp_err_t core_submit_cmd", self.core_c)
        self.assertIn("handle_service_cmd", self.core_fsm_c)
        self.assertIn("modem_mqtt_configure", self.core_fsm_c)
        self.assertIn("modem_mqtt_publish", self.core_fsm_c)

    def test_core_dispatches_cached_mqtt_commands_and_tcp_disconnect(self):
        self.assertIn("CORE_CMD_MQTT_TCP_DISCONNECT", self.core_h)
        self.assertIn("modem_mqtt_tcp_connect(me->modem);", self.core_fsm_c)
        self.assertIn("modem_mqtt_connect(me->modem);", self.core_fsm_c)
        self.assertIn("modem_mqtt_tcp_disconnect(me->modem);", self.core_fsm_c)

        mqtt_submit_start = self.mqtt_c.rindex("static esp_err_t submit_core_cmd")
        mqtt_submit_body = self.mqtt_c[
            mqtt_submit_start:
            self.mqtt_c.index("static esp_err_t begin_connect", mqtt_submit_start)
        ]
        for token in [
            "cmd.data.mqtt_config.client_id = me->config.auth.client_id;",
            "cmd.data.mqtt_config.username = me->config.auth.username;",
            "cmd.data.mqtt_config.password = me->config.auth.password;",
            "cmd.data.mqtt_config.host = me->config.endpoint.host;",
            "cmd.data.mqtt_config.port = me->config.endpoint.port;",
            "cmd.data.mqtt_config.clean_session = me->config.session.clean_session;",
            "cmd.data.mqtt_config.keepalive_s = me->config.session.keepalive_s;",
        ]:
            self.assertIn(token, mqtt_submit_body)

        core_cmd_match = re.search(r"typedef struct \{.*?\} core_cmd_t;", self.core_h, re.S)
        self.assertIsNotNone(core_cmd_match)
        core_cmd_body = core_cmd_match.group(0)
        mqtt_config_match = re.search(r"struct \{.*?\} mqtt_config;", core_cmd_body, re.S)
        self.assertIsNotNone(mqtt_config_match)
        mqtt_config_body = mqtt_config_match.group(0)
        for token in [
            "const char *client_id;",
            "const char *username;",
            "const char *password;",
            "const char *host;",
            "uint16_t port;",
            "bool clean_session;",
            "uint16_t keepalive_s;",
        ]:
            self.assertIn(token, mqtt_config_body)

        core_mqtt_sources = self.core_h + self.core_c + self.core_fsm_c + self.mqtt_c
        self.assertNotIn("mqtt_tcp_connect" ".host", core_mqtt_sources)
        self.assertNotIn("mqtt_connect" ".clean_session", core_mqtt_sources)
        self.assertNotIn("cmd.data.mqtt_tcp_connect", core_mqtt_sources)
        self.assertNotIn("cmd.data.mqtt_connect", core_mqtt_sources)
        self.assertNotIn("} mqtt_tcp_connect;", core_mqtt_sources)
        self.assertNotIn("} mqtt_connect;", core_mqtt_sources)

    def test_core_posts_to_lwlte_event_base(self):
        """Core must post events to the LWLTE_EVENT base."""
        self.assertIn("LWLTE_EVENT", self.core_c)
        self.assertNotIn("CORE_EVENT", self.core_h)
        self.assertIn("ESP_EVENT_DEFINE_BASE(LWLTE_EVENT)", self.core_c)
        self.assertIn("core_post_event", self.core_c)
        self.assertIn("LWLTE_EVENT_STARTED", self.core_fsm_c)
        self.assertIn("LWLTE_EVENT_READY", self.core_fsm_c)
        self.assertIn("LWLTE_EVENT_ERROR", self.core_fsm_c)

    def test_core_command_queue_ownership_and_cleanup_contract(self):
        for token in [
            "static char *clone_optional_string",
            "static uint8_t *clone_payload",
            "static bool core_cmd_valid",
            "CORE_CMD_MQTT_PUBLISH",
            "payload_len > 0",
            "qos <= 2",
            "core_free_cmd(cloned_cmd);",
            "static void release_modem_protocol_payload",
            "core_post_event",
            "ESP_EVENT_DEFINE_BASE(LWLTE_EVENT)",
        ]:
            self.assertIn(token, self.core_c)

        self.assertNotIn("core_release_event_payload", self.core_h)
        self.assertNotIn("core_event_adapter", self.core_c)
        self.assertNotIn("wait_event_callbacks_idle", self.core_c)

        fsm_send_body = self.core_fsm_c[
            self.core_fsm_c.index("esp_err_t core_fsm_send"):
            self.core_fsm_c.index("bool core_fsm_is_task")
        ]
        for token in [
            "xSemaphoreTake(me->lock",
            "me->fsm.stop_requested",
            "!me->fsm.task",
            "!me->fsm.queue",
            "xQueueSend(me->fsm.queue, sig, 0)",
        ]:
            self.assertIn(token, fsm_send_body)
        self.assertLess(
            fsm_send_body.index("me->fsm.stop_requested"),
            fsm_send_body.index("xQueueSend(me->fsm.queue, sig, 0)"),
        )

        fsm_deinit_body = self.core_fsm_c[
            self.core_fsm_c.index("esp_err_t core_fsm_deinit"):
            self.core_fsm_c.index("esp_err_t core_fsm_send")
        ]
        for token in [
            "QueueHandle_t queue = me->fsm.queue;",
            "drain_fsm_queue_payloads(me, queue);",
            "me->fsm.queue = NULL;",
            "vQueueDelete(queue);",
        ]:
            self.assertIn(token, fsm_deinit_body)
        self.assertLess(
            fsm_deinit_body.index("xSemaphoreGive(me->lock);"),
            fsm_deinit_body.index("drain_fsm_queue_payloads(me, queue);"),
        )
        self.assertLess(
            fsm_deinit_body.index("drain_fsm_queue_payloads(me, queue);"),
            fsm_deinit_body.index("vQueueDelete(queue);"),
        )

        for token in [
            "CORE_SIG_SERVICE_CMD",
            "handle_service_cmd(me, sig->service_cmd);",
            "finish_service_cmd",
            "result_from_esp_err",
            "CORE_CMD_RESULT_TIMEOUT",
            "CORE_CMD_RESULT_INVALID_RESPONSE",
            "core_free_cmd(cmd);",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "release_modem_protocol_payload(&sig->modem_event);",
            "finish_service_cmd(me, sig->service_cmd, CORE_CMD_RESULT_ERROR, NULL);",
        ]:
            self.assertIn(token, self.core_fsm_c)

        release_body = self.core_fsm_c[
            self.core_fsm_c.index("static void release_fsm_signal_payload"):
            self.core_fsm_c.index("static void drain_fsm_queue_payloads")
        ]
        self.assertNotIn("core_free_cmd(sig->service_cmd);", release_body)

    def test_modem_mqtt_ops_and_air780ep_commands_exist(self):
        for token in [
            "modem_mqtt_config_t",
            "modem_mqtt_topic_t",
            "modem_mqtt_publish_t",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_EVENT_PROTOCOL_CLOSED",
            "MODEM_PROTOCOL_MQTT",
            "modem_mqtt_configure(modem_handle_t me",
            "modem_mqtt_tcp_connect(modem_handle_t me",
            "modem_mqtt_connect(modem_handle_t me",
            "modem_mqtt_publish(modem_handle_t me",
        ]:
            self.assertIn(token, self.modem_h)

        old_modem_h_tokens = [
            "modem_mqtt_" + "open_t",
            "modem_mqtt_" + "login_t",
            "modem_mqtt_" + "config(modem_t *me",
            "modem_mqtt_" + "open(modem_t *me",
            "modem_mqtt_" + "login(modem_t *me",
        ]
        for token in old_modem_h_tokens:
            self.assertNotIn(token, self.modem_h)

        for token in [
            "mqtt_configure",
            "mqtt_tcp_connect",
            "mqtt_connect",
            "mqtt_disconnect",
            "mqtt_subscribe",
            "mqtt_unsubscribe",
            "mqtt_publish",
        ]:
            self.assertIn(token, self.modem_priv)

        old_modem_priv_tokens = [
            "mqtt_" + "config)(",
            "mqtt_" + "open",
            "mqtt_" + "login",
        ]
        for token in old_modem_priv_tokens:
            self.assertNotIn(token, self.modem_priv)

        for token in [
            "esp_err_t modem_mqtt_configure",
            "esp_err_t modem_mqtt_tcp_connect",
            "esp_err_t modem_mqtt_connect",
            "esp_err_t modem_mqtt_publish",
            "release_event_payload",
        ]:
            self.assertIn(token, self.modem_c)

        old_modem_c_tokens = [
            "esp_err_t modem_mqtt_" + "config(",
            "esp_err_t modem_mqtt_" + "open",
            "esp_err_t modem_mqtt_" + "login",
        ]
        for token in old_modem_c_tokens:
            self.assertNotIn(token, self.modem_c)

        for token in [
            "AIR780EP_URC_MSUB",
            "AT+MCONFIG",
            "AT+MIPSTART",
            "AT+MCONNECT",
            "AT+MDISCONNECT",
            "AT+MSUB",
            "AT+MUNSUB",
            "AT+MPUBEX",
            "air780ep_mqtt_configure",
            "air780ep_mqtt_tcp_connect",
            "air780ep_mqtt_connect",
            "air780ep_mqtt_publish",
            "handle_msub_urc",
        ]:
            self.assertIn(token, self.air780ep_c)

        old_air780ep_tokens = [
            "air780ep_mqtt_" + "config(",
            "air780ep_mqtt_" + "open",
            "air780ep_mqtt_" + "login",
        ]
        for token in old_air780ep_tokens:
            self.assertNotIn(token, self.air780ep_c)

    def test_modem_mqtt_config_is_unified_and_cached(self):
        config_match = re.search(
            r"typedef\s+struct\s*\{(?P<body>[^{}]*)\}\s*modem_mqtt_config_t;",
            self.modem_h,
        )
        self.assertIsNotNone(config_match, "missing modem_mqtt_config_t typedef")
        config_body = config_match.group("body")
        self.assertIn("const char *client_id;", config_body)
        self.assertIn("const char *username;", config_body)
        self.assertIn("const char *password;", config_body)
        self.assertIn("const char *host;", config_body)
        self.assertIn("uint16_t port;", config_body)
        self.assertIn("bool clean_session;", config_body)
        self.assertIn("uint16_t keepalive_s;", config_body)

        self.assertNotIn("modem_mqtt_tcp_" "config_t", self.modem_h + self.modem_priv + self.modem_c + self.air780ep_c)
        self.assertNotIn("modem_mqtt_connect_" "config_t", self.modem_h + self.modem_priv + self.modem_c + self.air780ep_c)

        self.assertIn("esp_err_t modem_mqtt_tcp_connect(modem_handle_t me);", self.modem_h)
        self.assertIn("esp_err_t modem_mqtt_connect(modem_handle_t me);", self.modem_h)
        self.assertIn("esp_err_t modem_mqtt_tcp_disconnect(modem_handle_t me);", self.modem_h)
        self.assertIn("esp_err_t modem_mqtt_tcp_disconnect(modem_handle_t me)", self.modem_c)
        self.assertIn("mqtt_tcp_disconnect", self.modem_priv)
        self.assertIn(".mqtt_tcp_disconnect = air780ep_mqtt_tcp_disconnect", self.air780ep_c)
        self.assertIn("static esp_err_t air780ep_mqtt_tcp_disconnect(modem_handle_t me)", self.air780ep_c)

        tcp_disconnect_start = self.air780ep_c.rindex("static esp_err_t air780ep_mqtt_tcp_disconnect")
        tcp_disconnect_end = self.air780ep_c.find("\nstatic ", tcp_disconnect_start + 1)
        if tcp_disconnect_end < 0:
            tcp_disconnect_end = len(self.air780ep_c)
        tcp_disconnect_body = self.air780ep_c[tcp_disconnect_start:tcp_disconnect_end]
        self.assertIn("AT+MIPCLOSE", tcp_disconnect_body)
        self.assertIn('ensure_at_ok(&ctx.response, "AT+MIPCLOSE")', tcp_disconnect_body)

        self.assertIn("mqtt_configured", self.air780ep_c)
        self.assertIn("mqtt_tcp_connected", self.air780ep_c)
        self.assertIn("mqtt_session_connected", self.air780ep_c)
        self.assertIn("free_mqtt_config", self.air780ep_c)

    def test_modem_protocol_data_event_cleanup_contract(self):
        self.assertIn("static void drain_event_queue_payloads", self.modem_c)
        self.assertIn("drain_event_queue_payloads(me);", self.modem_c)
        self.assertIn("xQueueReceive(me->event_queue, &event, 0)", self.modem_c)
        self.assertIn("release_event_payload(&event);", self.modem_c)

        deinit_body = self.modem_c[
            self.modem_c.index("esp_err_t modem_base_deinit"):
            self.modem_c.index("esp_err_t modem_base_stop_event_task")
        ]
        for token in [
            "QueueHandle_t event_queue = me->event_queue;",
            "drain_event_queue_payloads(me);",
            "me->event_queue = NULL;",
            "vQueueDelete(event_queue);",
        ]:
            self.assertIn(token, deinit_body)
        self.assertLess(
            deinit_body.index("drain_event_queue_payloads(me);"),
            deinit_body.index("me->event_queue = NULL;"),
        )
        self.assertLess(
            deinit_body.index("me->event_queue = NULL;"),
            deinit_body.index("vQueueDelete(event_queue);"),
        )

    def test_modem_post_event_rejects_stopped_event_task(self):
        post_event_body = self.modem_c[
            self.modem_c.index("esp_err_t modem_post_event"):
            self.modem_c.index("esp_err_t modem_set_state")
        ]

        for token in [
            "me->event_task_stop_requested",
            "!me->event_task",
            "!me->event_queue",
            "xQueueSend(me->event_queue, event, 0)",
        ]:
            self.assertIn(token, post_event_body)

        self.assertLess(
            post_event_body.index("me->event_task_stop_requested"),
            post_event_body.index("xQueueSend(me->event_queue, event, 0)"),
        )

    def test_modem_protocol_event_ids_keep_error_stable(self):
        event_enum = self.modem_h[
            self.modem_h.index("MODEM_EVENT_READY"):
            self.modem_h.index("} modem_event_id_t;")
        ]
        event_ids = []
        event_values = {}
        next_event_value = 0
        for line in event_enum.splitlines():
            line = line.strip()
            if line.startswith("MODEM_EVENT_"):
                token = line.split(",", 1)[0]
                if "=" in token:
                    name, value = token.split("=", 1)
                    name = name.strip()
                    next_event_value = int(value.strip(), 0)
                else:
                    name = token.strip()
                event_ids.append(name)
                event_values[name] = next_event_value
                next_event_value += 1

        self.assertEqual([
            "MODEM_EVENT_READY",
            "MODEM_EVENT_SIM_CHANGED",
            "MODEM_EVENT_REG_CHANGED",
            "MODEM_EVENT_PDP_ACTIVATED",
            "MODEM_EVENT_PDP_DEACTIVATED",
            "MODEM_EVENT_SIGNAL_CHANGED",
            "MODEM_EVENT_ERROR",
        ], event_ids[:7])

        error_pos = event_enum.index("MODEM_EVENT_ERROR")
        protocol_data_pos = event_enum.index("MODEM_EVENT_PROTOCOL_DATA")
        protocol_closed_pos = event_enum.index("MODEM_EVENT_PROTOCOL_CLOSED")

        self.assertLess(error_pos, protocol_data_pos)
        self.assertLess(protocol_data_pos, protocol_closed_pos)
        self.assertEqual(6, event_values["MODEM_EVENT_ERROR"])
        self.assertEqual(7, event_values["MODEM_EVENT_PROTOCOL_DATA"])
        self.assertEqual(8, event_values["MODEM_EVENT_PROTOCOL_CLOSED"])

    def test_modem_protocol_data_lifetime_contract_is_documented(self):
        for token in [
            "MODEM_EVENT_PROTOCOL_DATA",
            "heap-owned",
            "modem_post_event() succeeds",
            "caller keeps ownership",
            "valid only during modem_event_callback_t",
            "copy topic/payload",
        ]:
            self.assertIn(token, self.modem_h)

    def test_at_engine_payload_prompt_support_exists(self):
        self.assertIn("at_engine_send_cmd_with_payload", self.at_engine_h)
        self.assertIn("const uint8_t *payload", self.at_engine_h)
        self.assertIn("payload_prompt", self.at_engine_c)
        self.assertIn("write_payload", self.at_engine_c)
        self.assertIn("uart_write_bytes", self.at_engine_c)

    def test_at_engine_uses_grouped_config_as_single_source(self):
        match = re.search(r"struct\s+at_engine_t\s*\{(?P<body>[\s\S]*?)\};", self.at_engine_c)
        self.assertIsNotNone(match, "missing struct at_engine definition")
        body = match.group("body")
        self.assertNotIn("uart_port_t uart_num", body)
        for token in [
            "} at_engine_uart_config_t;",
            "} at_engine_runtime_config_t;",
            "at_engine_uart_config_t uart;",
            "at_engine_runtime_config_t runtime;",
            "me->config.uart.uart_num",
            "me->config.uart.rx_buf_size",
            "me->config.runtime.rx_task_stack",
            "me->config.runtime.rx_line_buf_size",
            "me->config.runtime.max_response_lines",
        ]:
            self.assertIn(token, self.at_engine_h + self.at_engine_c)

    def test_core_uses_grouped_config_as_single_source(self):
        for token in [
            "} core_event_config_t;",
            "} core_network_config_t;",
            "} core_fsm_config_t;",
            "core_event_config_t event;",
            "core_network_config_t network;",
            "core_fsm_config_t fsm;",
            "me->config.event.loop",
            "me->config.network.apn",
            "me->config.network.primary_cid",
            "me->config.fsm.queue_size",
        ]:
            self.assertIn(token, self.core_h + self.core_c + self.core_fsm_c)

    def test_mqtt_client_uses_grouped_config_as_single_source(self):
        for token in [
            "} mqtt_client_endpoint_config_t;",
            "} mqtt_client_auth_config_t;",
            "} mqtt_client_session_config_t;",
            "} mqtt_client_fsm_config_t;",
            "} mqtt_client_event_config_t;",
            "mqtt_client_endpoint_config_t endpoint;",
            "mqtt_client_auth_config_t auth;",
            "mqtt_client_session_config_t session;",
            "mqtt_client_fsm_config_t fsm;",
            "mqtt_client_event_config_t event;",
            "config->endpoint.host",
            "config->endpoint.port",
            "config->auth.client_id",
            "config->endpoint.transport",
            "me->config.event.loop",
            "me->config.fsm.queue_size",
        ]:
            self.assertIn(token, self.mqtt_h + self.mqtt_c)

    def test_protocol_data_path_symbols_exist(self):
        self.assertIn("MODEM_EVENT_PROTOCOL_DATA", self.air780ep_c)
        self.assertIn("clone_modem_protocol_payload", self.core_c)
        self.assertIn("protocol_callback", self.core_fsm_c)
        self.assertIn("mqtt_protocol_data_cb", self.mqtt_c)
        self.assertIn("LWLTE_MQTT_EVENT_DATA", self.mqtt_c)
        self.assertIn("lwlte_mqtt_event_data_release", self.lwlte_c)
        self.assertIn("facade_ready_handler", self.lwlte_priv)
        self.assertIn("esp_event_handler_register", self.lwlte_air780ep_c)

    def test_facade_mqtt_wrappers_use_mqtt_client_layer_only(self):
        self.assertIn("esp_err_t lwlte_mqtt_start", self.lwlte_c)
        self.assertIn("esp_err_t lwlte_mqtt_init", self.lwlte_c)

        def facade_api_body(start_marker, end_marker):
            start = self.lwlte_c.index(start_marker)
            end = self.lwlte_c.index(end_marker, start + len(start_marker))
            return self.lwlte_c[start:end]

        api_body = "".join([
            facade_api_body("esp_err_t lwlte_mqtt_init",
                            "esp_err_t lwlte_mqtt_destroy"),
            facade_api_body("esp_err_t lwlte_mqtt_destroy",
                            "esp_err_t lwlte_tcp_init"),
            facade_api_body("esp_err_t lwlte_mqtt_start",
                            "esp_err_t lwlte_mqtt_stop"),
            facade_api_body("esp_err_t lwlte_mqtt_stop",
                            "esp_err_t lwlte_mqtt_get_state"),
            facade_api_body("esp_err_t lwlte_mqtt_get_state",
                            "esp_err_t lwlte_mqtt_subscribe"),
            facade_api_body("esp_err_t lwlte_mqtt_subscribe",
                            "esp_err_t lwlte_mqtt_unsubscribe"),
            facade_api_body("esp_err_t lwlte_mqtt_unsubscribe",
                            "esp_err_t lwlte_mqtt_publish"),
            facade_api_body("esp_err_t lwlte_mqtt_publish",
                            "esp_err_t lwlte_wait_ready"),
        ])

        for token in [
            "static esp_err_t begin_mqtt_api_call",
            "mqtt_client_start(mqtt)",
            "mqtt_client_stop(mqtt)",
            "mqtt_client_get_state(mqtt, &mqtt_state)",
            "mqtt_client_subscribe(mqtt, topic, qos)",
            "mqtt_client_unsubscribe(mqtt, topic)",
            "mqtt_client_publish(mqtt, &request)",
            "map_mqtt_state(mqtt_state)",
            "mqtt_client_create(&mqtt_config, core)",
        ]:
            self.assertIn(token, self.lwlte_c)

        for token in [
            ".endpoint = {",
            ".transport = MQTT_CLIENT_TRANSPORT_PLAIN_TCP",
            ".host = config->host",
            ".port = config->port",
            ".auth = {",
            ".client_id = config->client_id",
            ".username = config->username",
            ".password = config->password",
            ".session = {",
            ".keepalive_s = config->keepalive_s",
            ".clean_session = config->clean_session",
            ".fsm = {",
            ".queue_size = config->fsm_queue_size",
            ".task_stack = config->fsm_task_stack",
            ".task_priority = config->fsm_task_priority",
            ".event = {",
            ".loop = me->event_loop",
        ]:
            self.assertIn(token, api_body)

        for forbidden in [
            "core_submit_cmd",
            "CORE_CMD_MQTT_CONFIGURE",
            "CORE_CMD_MQTT_TCP_CONNECT",
            "CORE_CMD_MQTT_CONNECT",
            "CORE_CMD_MQTT_SUBSCRIBE",
            "CORE_CMD_MQTT_UNSUBSCRIBE",
            "CORE_CMD_MQTT_PUBLISH",
            "CORE_CMD_MQTT_DISCONNECT",
        ]:
            self.assertNotIn(forbidden, api_body)

    def test_facade_mqtt_destroyed_before_core(self):
        destroy_body = self.lwlte_c[
            self.lwlte_c.rindex("static esp_err_t destroy_owned_resources"):
        ]

        self.assertIn("mqtt_client_destroy(me->mqtt)", destroy_body)
        self.assertIn("me->mqtt = NULL;", destroy_body)
        self.assertLess(
            destroy_body.index("mqtt_client_destroy(me->mqtt)"),
            destroy_body.index("core_destroy(me->core)")
        )

    def test_mqtt_null_event_data_carries_current_state_before_dispatch(self):
        post_start = self.mqtt_c.rindex("static esp_err_t post_mqtt_event")
        post_body = self.mqtt_c[
            post_start:
            self.mqtt_c.index("static void post_error_event", post_start)
        ]
        null_data_body = post_body[
            post_body.index("if (!payload) {"):
            post_body.index("esp_err_t ret;")
        ]

        for token in [
            "xSemaphoreTake(me->lock, portMAX_DELAY);",
            "empty_payload.mqtt_state = map_mqtt_state(me->state);",
            "xSemaphoreGive(me->lock);",
            "payload = &empty_payload;",
        ]:
            self.assertIn(token, null_data_body)

        self.assertLess(
            null_data_body.index("xSemaphoreTake(me->lock, portMAX_DELAY);"),
            null_data_body.index("empty_payload.mqtt_state = map_mqtt_state(me->state);")
        )
        self.assertLess(
            null_data_body.index("empty_payload.mqtt_state = map_mqtt_state(me->state);"),
            null_data_body.index("xSemaphoreGive(me->lock);")
        )
        self.assertLess(
            null_data_body.index("xSemaphoreGive(me->lock);"),
            null_data_body.index("payload = &empty_payload;")
        )

    def test_mqtt_data_event_clones_payload_for_bus(self):
        """MQTT DATA events must clone payload for the event bus and set owns_payload."""
        handle_start = self.mqtt_c.index("static void handle_protocol_data")
        handle_body = self.mqtt_c[
            handle_start:
            self.mqtt_c.index("/**********************\n *   GLOBAL FUNCTIONS")
        ]
        self.assertIn("clone_string(owned->topic)", handle_body)
        self.assertIn("clone_payload(owned->payload", handle_body)
        self.assertRegex(handle_body, r"\.owns_payload\s*=\s*true",
                         "owns_payload should be set to true for DATA events")
        self.assertIn("LWLTE_MQTT_EVENT_DATA", handle_body)
        self.assertIn("post_mqtt_event", handle_body)

    def test_air780ep_mqtt_config_validation_and_factory_wiring(self):
        validate_body = self.lwlte_air780ep_c[
            self.lwlte_air780ep_c.rindex("static esp_err_t validate_config"):
            self.lwlte_air780ep_c.rindex("static bool gpio_required_valid")
        ]
        for token in [
            "config->base.uart.num",
            "config->base.uart.tx_pin",
            "config->base.uart.rx_pin",
            "config->base.uart.baud_rate > 0",
            "config->base.core.primary_cid == LWLTE_AIR780EP_PRIMARY_CID",
            "non_negative_int(config->base.at_engine.rx_buf_size)",
            "non_negative_int(config->base.at_engine.rx_task_stack)",
            "non_negative_int(config->base.at_engine.rx_task_priority)",
        ]:
            self.assertIn(token, validate_body)

        init_body = self.lwlte_air780ep_c[
            self.lwlte_air780ep_c.index("esp_err_t lwlte_air780ep_init"):
            self.lwlte_air780ep_c.index("*out_lte = me;")
        ]
        for token in [
            "at_engine_create(&at_config)",
            "modem_air780ep_create(me->at, &modem_config)",
            "core_create(&core_config, me->modem)",
            "me->event_loop = config->base.event.loop",
            "esp_event_handler_register_with(me->event_loop, LWLTE_EVENT,",
            "facade_ready_handler, me",
            "ping_client_create(me->core)",
            "cleanup_after_failure(me, ret)",
        ]:
            self.assertIn(token, init_body)

        for token in [
            ".uart = {",
            ".uart_num = config->base.uart.num",
            ".tx_pin = config->base.uart.tx_pin",
            ".rx_pin = config->base.uart.rx_pin",
            ".baud_rate = config->base.uart.baud_rate",
            ".rx_buf_size = config->base.at_engine.rx_buf_size",
            ".runtime = {",
            ".rx_task_stack = config->base.at_engine.rx_task_stack",
            ".rx_task_priority = config->base.at_engine.rx_task_priority",
            ".rx_line_buf_size = at_rx_line_buf_size",
            ".cmd_default_timeout_ms = config->base.at_engine.cmd_default_timeout_ms",
            ".max_response_lines = config->base.at_engine.max_response_lines",
        ]:
            self.assertIn(token, self.lwlte_air780ep_c)
        for token in [
            "int at_rx_line_buf_size = config->base.at_engine.rx_line_buf_size ?",
            "config->base.at_engine.rx_line_buf_size :",
            "LWLTE_AIR780EP_DEFAULT_AT_LINE_BUF_SIZE",
        ]:
            self.assertIn(token, self.lwlte_air780ep_c)

        for token in [
            ".event = {",
            ".loop = me->event_loop",
            ".network = {",
            ".apn = config->base.core.apn ? config->base.core.apn : \"\"",
            ".primary_cid = config->base.core.primary_cid",
            ".net_activate_timeout_ms = config->base.core.net_activate_timeout_ms",
            ".reconnect_delay_ms = config->base.core.reconnect_delay_ms",
            ".fsm = {",
            ".queue_size = config->base.core.fsm_queue_size",
            ".task_stack = config->base.core.fsm_task_stack",
            ".task_priority = config->base.core.fsm_task_priority",
        ]:
            self.assertIn(token, self.lwlte_air780ep_c)

        self.assertNotIn("core_register_event_callback", self.lwlte_air780ep_c)
        self.assertNotIn("mqtt_client_register_event_callback", self.lwlte_air780ep_c)
        self.assertNotIn("mqtt_client_create", self.lwlte_air780ep_c)
        self.assertNotIn("core_start", self.lwlte_air780ep_c)
        start_body = self.lwlte_c[
            self.lwlte_c.index("esp_err_t lwlte_start"):
            self.lwlte_c.index("esp_err_t lwlte_stop")
        ]
        self.assertIn("core_start(core)", start_body)

    def test_facade_mqtt_event_data_release_and_state_mapping(self):
        """Facade exposes mqtt event data release and maps mqtt state."""
        self.assertIn("void lwlte_mqtt_event_data_release", self.lwlte_c)
        release_body = self.lwlte_c[
            self.lwlte_c.index("void lwlte_mqtt_event_data_release"):
            self.lwlte_c.index("esp_err_t lwlte_create_empty")
        ]
        self.assertIn("data->owns_payload", release_body)
        self.assertIn("free((void *)data->msg.topic)", release_body)
        self.assertIn("free((void *)data->msg.payload)", release_body)

        self.assertIn("static lwlte_mqtt_state_t map_mqtt_state", self.lwlte_c)
        map_body = self.lwlte_c[
            self.lwlte_c.rindex("static lwlte_mqtt_state_t map_mqtt_state"):
            self.lwlte_c.rindex("static esp_err_t begin_api_call")
        ]
        self.assertIn("MQTT_CLIENT_STATE_CONNECTED", map_body)
        self.assertIn("LWLTE_MQTT_STATE_CONNECTED", map_body)

        self.assertIn("ESP_EVENT_DEFINE_BASE(LWLTE_MQTT_EVENT)", self.lwlte_c)

    def test_air780ep_mqtt_command_and_urc_ownership_contract(self):
        for token in [
            "AIR780EP_MQTT_PAYLOAD_PROMPT",
            "AT+MCONFIG=\\\"%s\\\",\\\"%s\\\",\\\"%s\\\"",
            "AT+MIPSTART",
            "AT+SSLMIPSTART",
            "AT+MCONNECT=%u,%u",
            "AT+MDISCONNECT",
            "AT+MSUB=\\\"%s\\\",%u",
            "AT+MUNSUB=\\\"%s\\\"",
            "AT+MPUBEX=\\\"%s\\\",%u,%u,%u",
            "CONNECT OK",
            "ALREADY CONNECT",
            "CONNACK OK",
            "SUBACK",
            "UNSUBACK",
            "PUBACK",
            "AT_CMD_FLAG_NO_STANDARD_OK_FINAL | AT_CMD_FLAG_SKIP_INTERMEDIATE_OK",
            "at_engine_send_cmd_with_payload",
            "AIR780EP_MQTT_PAYLOAD_PROMPT",
            "MODEM_EVENT_PROTOCOL_DATA",
            "MODEM_PROTOCOL_MQTT",
            "modem_post_event",
            "free(topic)",
            "free(payload)",
            "parse_msub_direct",
            "post_mqtt_data_event",
        ]:
            self.assertIn(token, self.air780ep_c)

    def test_mqtt_destroy_waits_for_graceful_stop_before_freeing_client(self):
        for token in [
            "SemaphoreHandle_t stop_done_sema;",
            "#define MQTT_CLIENT_STOP_WAIT_MS",
            "static esp_err_t wait_stop_before_destroy",
            "wait_stop_before_destroy(me)",
            "xSemaphoreGive(me->stop_done_sema);",
            "ESP_ERR_TIMEOUT",
        ]:
            self.assertIn(token, self.mqtt_c + self.mqtt_priv)

        destroy_start = self.mqtt_c.rindex("esp_err_t mqtt_client_destroy")
        destroy_body = self.mqtt_c[
            destroy_start:
            self.mqtt_c.index("esp_err_t mqtt_client_start", destroy_start)
        ]
        self.assertIn("wait_stop_before_destroy(me)", destroy_body)
        self.assertIn("return ret;", destroy_body)
        self.assertIn("me->destroying = true;", destroy_body)
        self.assertIn("me->state = MQTT_CLIENT_STATE_DESTROYING;", destroy_body)
        self.assertIn("free(me);", destroy_body)
        self.assertLess(
            destroy_body.index("wait_stop_before_destroy(me)"),
            destroy_body.index("me->destroying = true;")
        )
        self.assertLess(
            destroy_body.index("if (ret != ESP_OK)"),
            destroy_body.index("me->destroying = true;")
        )
        self.assertLess(
            destroy_body.index("me->destroying = true;"),
            destroy_body.index("free(me);")
        )

        wait_start = self.mqtt_c.rindex("static esp_err_t wait_stop_before_destroy")
        wait_body = self.mqtt_c[
            wait_start:
            self.mqtt_c.index("static esp_err_t post_mqtt_event", wait_start)
        ]
        self.assertIn("send_simple_sig(me, MQTT_SIG_STOP)", wait_body)
        self.assertIn("xSemaphoreTake(done_sema", wait_body)
        self.assertIn("pdMS_TO_TICKS(MQTT_CLIENT_STOP_WAIT_MS)", wait_body)
        self.assertNotIn("core_submit_cmd", wait_body)

    def test_air780ep_drops_mqtt_urc_payloads_unless_mqtt_data_enabled(self):
        for token in [
            "bool mqtt_data_enabled;",
            "static void set_mqtt_data_enabled",
            "static bool mqtt_data_is_enabled",
            "mqtt_data_is_enabled(self)",
        ]:
            self.assertIn(token, self.air780ep_c)

        connect_anchor = "static esp_err_t air780ep_mqtt_connect"
        self.assertIn(connect_anchor, self.air780ep_c)
        connect_body = self.air780ep_c[
            self.air780ep_c.rindex(connect_anchor):
            self.air780ep_c.rindex("static esp_err_t air780ep_mqtt_disconnect")
        ]
        connect_enable_marker = (
            "set_mqtt_data_enabled(self, true);"
            if "set_mqtt_data_enabled(self, true);" in connect_body
            else "self->mqtt_data_enabled = true;"
        )
        self.assertIn(connect_enable_marker, connect_body)
        self.assertLess(
            connect_body.index("if (ret == ESP_OK)"),
            connect_body.index(connect_enable_marker)
        )

        disconnect_body = self.air780ep_c[
            self.air780ep_c.rindex("static esp_err_t air780ep_mqtt_disconnect"):
            self.air780ep_c.rindex("static esp_err_t air780ep_mqtt_subscribe")
        ]
        disconnect_disable_marker = (
            "set_mqtt_data_enabled(self, false);"
            if "set_mqtt_data_enabled(self, false);" in disconnect_body
            else "self->mqtt_data_enabled = false;"
        )
        self.assertIn(disconnect_disable_marker, disconnect_body)
        self.assertLess(
            disconnect_body.index(disconnect_disable_marker),
            disconnect_body.index("send_cmd(self, \"AT+MDISCONNECT\"")
        )
        self.assertIn("self->mqtt_session_connected = false;", disconnect_body)

        inactive_paths = [
            (
                "destroy",
                "static esp_err_t air780ep_destroy",
                "static esp_err_t air780ep_start",
            ),
            (
                "start",
                "static esp_err_t air780ep_start",
                "static esp_err_t air780ep_reset",
            ),
            (
                "reset",
                "static esp_err_t air780ep_reset",
                "static esp_err_t air780ep_get_info",
            ),
            (
                "PDP deactivate",
                "static esp_err_t air780ep_deactivate_pdp",
                "static esp_err_t air780ep_get_pdp_context",
            ),
            (
                "+CGEV PDP deactivation URC",
                "static void cgev_urc_handler",
                "static void pdp_deact_urc_handler",
            ),
            (
                "PDP deactivation URC",
                "static void pdp_deact_urc_handler",
                "static void handle_msub_urc",
            ),
        ]
        for name, start_token, end_token in inactive_paths:
            body = self.air780ep_c[
                self.air780ep_c.rindex(start_token):
                self.air780ep_c.rindex(end_token)
            ]
            self.assertTrue(
                "set_mqtt_data_enabled(self, false);" in body or
                "self->mqtt_data_enabled = false;" in body,
                f"{name} must disable MQTT data forwarding",
            )

        msub_body = self.air780ep_c[
            self.air780ep_c.rindex("static void handle_msub_urc"):
        ]
        self.assertIn("if (!mqtt_data_is_enabled(self))", msub_body)
        self.assertIn("free(topic);", msub_body)
        self.assertIn("free(payload);", msub_body)
        self.assertLess(
            msub_body.index("if (!mqtt_data_is_enabled(self))"),
            msub_body.index("post_mqtt_data_event")
        )


    def test_facade_has_no_callback_machinery(self):
        """The old callback slot and sync machinery must be deleted."""
        self.assertNotIn("callback_done_sema", self.lwlte_priv)
        self.assertNotIn("event_callback", self.lwlte_priv)
        self.assertNotIn("callback_active", self.lwlte_priv)
        self.assertNotIn("LWLTE_CALLBACK_TASKS_MAX", self.lwlte_priv)
        self.assertNotIn("lwlte_register_event_callback", self.lwlte_c)
        self.assertNotIn("lwlte_handle_core_event", self.lwlte_c)
        self.assertNotIn("lwlte_handle_mqtt_event", self.lwlte_c)
        self.assertNotIn("wait_callbacks_idle", self.lwlte_c)

    def test_new_event_bus_contract_exists(self):
        """The new event bus contract must be present."""
        self.assertIn("ESP_EVENT_DECLARE_BASE(LWLTE_EVENT)", self.lwlte_h)
        self.assertIn("ESP_EVENT_DECLARE_BASE(LWLTE_MQTT_EVENT)", self.lwlte_h)
        self.assertIn("lwlte_mqtt_event_data_release", self.lwlte_h)
        self.assertIn("lwlte_event_id_t", self.lwlte_h)
        self.assertIn("lwlte_mqtt_event_id_t", self.lwlte_h)

    def test_core_has_protocol_callback_api(self):
        """Core must expose the private protocol callback API."""
        self.assertIn("core_register_protocol_callback", self.core_h)
        self.assertIn("core_register_protocol_closed_callback", self.core_h)

    def test_core_has_no_private_event_loop(self):
        """Core must not have its own event loop."""
        self.assertNotIn("create_event_loop", self.core_c)
        self.assertNotIn("core_event_adapter", self.core_c)
        self.assertNotIn("CORE_EVENT_QUEUE_SIZE", self.core_priv)

    def test_mqtt_client_has_no_private_event_loop(self):
        """MQTT client must not have its own event loop or callback API."""
        self.assertNotIn("create_event_loop", self.mqtt_c)
        self.assertNotIn("mqtt_client_register_event_callback", self.mqtt_h)
        self.assertNotIn("mqtt_client_get_event_loop", self.mqtt_h)

    def test_event_loop_field_in_configs(self):
        """Both lwlte config structs must carry typed event loop via base.event."""
        event_match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                                r"lwlte_event_config_t\s*;",
                                self.lwlte_h, re.DOTALL)
        self.assertIsNotNone(event_match, "lwlte_event_config_t typedef not found in lwlte.h")
        self.assertIn("esp_event_loop_handle_t loop", event_match.group("body"),
                      "lwlte_event_config_t missing loop field")

        base_match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                               r"lwlte_base_config_t\s*;",
                               self.lwlte_h, re.DOTALL)
        self.assertIsNotNone(base_match, "lwlte_base_config_t typedef not found in lwlte.h")
        self.assertRegex(base_match.group("body"), r"lwlte_event_config_t\s+event;",
                         "lwlte_base_config_t missing event config field")

        for config_name in ["lwlte_air780ep_config_t", "lwlte_ml307r_config_t"]:
            match = re.search(r"typedef\s+struct\s*\{(?P<body>[^{}]*?)\}\s*"
                              + config_name + r"\s*;",
                              self.lwlte_h, re.DOTALL)
            self.assertIsNotNone(match, f"{config_name} typedef not found in lwlte.h")
            self.assertRegex(match.group("body"), r"lwlte_base_config_t\s+base;",
                             f"{config_name} missing base config field")


if __name__ == "__main__":
    unittest.main()
