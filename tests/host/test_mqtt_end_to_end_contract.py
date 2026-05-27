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
        cls.air780ep_h = read_optional(AIR780EP_H)
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

    def test_mqtt_service_owns_signal_payloads_and_callback_lifecycle(self):
        for token in [
            "typedef struct mqtt_protocol_data_owned",
            "event_callback_active",
            "event_callback_task",
            "event_callback_done_sema",
            "event_callback_waiting",
            "static void free_mqtt_fsm_sig_payload",
            "wait_event_callbacks_idle",
            "mqtt_client_destroy",
            "mqtt_fsm_task",
            "me->fsm_task == xTaskGetCurrentTaskHandle()",
            "me->event_callback_task == xTaskGetCurrentTaskHandle()",
            "core_release_event_payload((core_event_data_t *)data);",
            "MQTT_CLIENT_EVENT_DATA",
        ]:
            self.assertIn(token, self.mqtt_c + self.mqtt_priv)

        for token in [
            "clone_string",
            "clone_payload",
            "xQueueSend(me->fsm_queue, &sig, 0)",
            "ESP_ERR_TIMEOUT",
        ]:
            self.assertIn(token, self.mqtt_c)

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
            self.mqtt_c.index("static void mqtt_fsm_task"):
            self.mqtt_c.index("esp_err_t mqtt_client_register_event_callback")
        ]
        for token in [
            "submit_core_cmd(me, CORE_CMD_MQTT_CONFIG",
            "submit_core_cmd(me, CORE_CMD_MQTT_OPEN",
            "submit_core_cmd(me, CORE_CMD_MQTT_LOGIN",
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
            "static bool should_disconnect_for_stop",
            "static void request_stop_disconnect",
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
        done_start = self.mqtt_c.rindex("static void handle_core_cmd_done")
        done_body = self.mqtt_c[
            done_start:
            self.mqtt_c.index("static void handle_runtime_operation", done_start)
        ]
        for token in [
            "if (me->stop_requested) {",
            "if (operation == MQTT_CLIENT_OPERATION_DISCONNECT) {",
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
            done_body.index("MQTT_CLIENT_EVENT_SUBSCRIBED")
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
            signal_body.index("MQTT_CLIENT_EVENT_DISCONNECTED")
        )

    def test_mqtt_data_event_uses_direct_callback_only_for_signal_owned_payloads(self):
        post_start = self.mqtt_c.rindex("static esp_err_t post_mqtt_event")
        post_body = self.mqtt_c[
            post_start:
            self.mqtt_c.index("static void post_error_event", post_start)
        ]
        self.assertIn("if (event_id != MQTT_CLIENT_EVENT_DATA) {", post_body)
        self.assertIn("esp_event_post_to", post_body)
        self.assertLess(
            post_body.index("if (event_id != MQTT_CLIENT_EVENT_DATA) {"),
            post_body.index("esp_event_post_to")
        )

        data_branch = post_body[post_body.index("if (event_id != MQTT_CLIENT_EVENT_DATA) {"):]
        self.assertIn("callback(me, event_id, event_data, user_ctx);", data_branch)
        self.assertNotIn(
            "esp_event_post_to(me->event_loop, MQTT_CLIENT_EVENT, event_id",
            data_branch[data_branch.index("callback(me, event_id, event_data, user_ctx);"):]
        )

        for token in [
            "MQTT_CLIENT_EVENT_DATA is dispatched only through mqtt_client_event_callback_t",
            "signal-owned topic/payload are freed after the direct callback returns",
        ]:
            self.assertIn(token, self.classes_md)

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

    def test_core_protocol_event_ids_keep_stopped_error_stable(self):
        event_enum = self.core_h[
            self.core_h.index("CORE_EVENT_STARTED"):
            self.core_h.index("} core_event_id_t;")
        ]
        event_ids = []
        event_values = {}
        next_event_value = 0
        for line in event_enum.splitlines():
            line = line.strip()
            if line.startswith("CORE_EVENT_"):
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
            "CORE_EVENT_STARTED",
            "CORE_EVENT_READY",
            "CORE_EVENT_NET_CONNECTING",
            "CORE_EVENT_NET_ONLINE",
            "CORE_EVENT_NET_OFFLINE",
            "CORE_EVENT_NET_ERROR",
            "CORE_EVENT_STOPPED",
            "CORE_EVENT_ERROR",
        ], event_ids[:8])

        self.assertIn("CORE_EVENT_PROTOCOL_DATA", event_enum)
        self.assertIn("CORE_EVENT_PROTOCOL_CLOSED", event_enum)

        error_pos = event_enum.index("CORE_EVENT_ERROR")
        protocol_data_pos = event_enum.index("CORE_EVENT_PROTOCOL_DATA")
        protocol_closed_pos = event_enum.index("CORE_EVENT_PROTOCOL_CLOSED")

        self.assertLess(error_pos, protocol_data_pos)
        self.assertLess(protocol_data_pos, protocol_closed_pos)
        self.assertEqual(6, event_values["CORE_EVENT_STOPPED"])
        self.assertEqual(7, event_values["CORE_EVENT_ERROR"])
        self.assertEqual(8, event_values["CORE_EVENT_PROTOCOL_DATA"])
        self.assertEqual(9, event_values["CORE_EVENT_PROTOCOL_CLOSED"])

    def test_core_command_queue_ownership_and_cleanup_contract(self):
        for token in [
            "static char *clone_optional_string",
            "static uint8_t *clone_payload",
            "static bool core_cmd_valid",
            "CORE_CMD_MQTT_PUBLISH",
            "payload_len > 0",
            "qos <= 2",
            "core_free_cmd(cloned_cmd);",
            "release_core_event_payload",
            "static void release_modem_protocol_payload",
            "core_post_protocol_data",
        ]:
            self.assertIn(token, self.core_c)

        self.assertIn("core_release_event_payload(core_event_data_t *event_data);", self.core_h)

        adapter_start = self.core_c.rindex("static void core_event_adapter")
        adapter_body = self.core_c[
            adapter_start:
            self.core_c.index("static esp_err_t wait_event_callbacks_idle", adapter_start)
        ]
        self.assertIn("event_id == CORE_EVENT_PROTOCOL_DATA", adapter_body)
        self.assertNotIn("release_core_event_payload", adapter_body)

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
            self.core_fsm_c.index("void core_fsm_deinit"):
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

    def test_modem_protocol_data_event_cleanup_contract(self):
        self.assertIn("static void drain_event_queue_payloads", self.modem_c)
        self.assertIn("drain_event_queue_payloads(me);", self.modem_c)
        self.assertIn("xQueueReceive(me->event_queue, &event, 0)", self.modem_c)
        self.assertIn("release_event_payload(&event);", self.modem_c)

        deinit_body = self.modem_c[
            self.modem_c.index("void modem_base_deinit"):
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

    def test_protocol_data_path_symbols_exist(self):
        self.assertIn("MODEM_EVENT_PROTOCOL_DATA", self.air780ep_c)
        self.assertIn("clone_protocol_data", self.core_c)
        self.assertIn("CORE_EVENT_PROTOCOL_DATA", self.core_c)
        self.assertIn("core_post_protocol_data", self.core_fsm_c)
        self.assertIn("handle_core_event", self.mqtt_c)
        self.assertIn("MQTT_CLIENT_EVENT_DATA", self.mqtt_c)
        self.assertIn("LWLTE_EVENT_MQTT_DATA", self.lwlte_c)
        self.assertIn("lwlte_handle_mqtt_event", self.lwlte_priv)
        self.assertIn("mqtt_client_register_event_callback", self.lwlte_air780ep_c)

    def test_facade_mqtt_wrappers_use_mqtt_client_layer_only(self):
        self.assertIn("esp_err_t lwlte_mqtt_start", self.lwlte_c)
        self.assertIn("void lwlte_handle_core_event", self.lwlte_c)
        api_start = self.lwlte_c.index("esp_err_t lwlte_mqtt_start")
        api_body = self.lwlte_c[
            api_start:
            self.lwlte_c.index("void lwlte_handle_core_event", api_start)
        ]

        for token in [
            "static esp_err_t begin_mqtt_api_call",
            "mqtt_client_start(mqtt)",
            "mqtt_client_stop(mqtt)",
            "mqtt_client_get_state(mqtt, &mqtt_state)",
            "mqtt_client_subscribe(mqtt, topic, qos)",
            "mqtt_client_unsubscribe(mqtt, topic)",
            "mqtt_client_publish(mqtt, &request)",
            "map_mqtt_state(mqtt_state)",
        ]:
            self.assertIn(token, self.lwlte_c)

        for forbidden in [
            "core_submit_cmd",
            "CORE_CMD_MQTT_CONFIG",
            "CORE_CMD_MQTT_OPEN",
            "CORE_CMD_MQTT_LOGIN",
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

    def test_facade_tracks_active_callback_tasks_individually(self):
        self.assertIn("TaskHandle_t callback_tasks[", self.lwlte_priv)
        self.assertIn("static bool callback_task_active_locked", self.lwlte_c)
        self.assertIn("static bool add_callback_task_locked", self.lwlte_c)
        self.assertIn("static void remove_callback_task_locked", self.lwlte_c)

        for function_name, next_function in [
            ("esp_err_t lwlte_destroy", "esp_err_t lwlte_register_event_callback"),
            ("esp_err_t lwlte_register_event_callback", "esp_err_t lwlte_connect"),
            ("static esp_err_t wait_callbacks_idle", "static bool callback_task_active_locked"),
        ]:
            function_start = self.lwlte_c.rindex(function_name)
            function_body = self.lwlte_c[
                function_start:
                self.lwlte_c.index(next_function, function_start)
            ]
            self.assertIn("callback_task_active_locked(me, xTaskGetCurrentTaskHandle())", function_body)
            self.assertNotIn("me->callback_task == xTaskGetCurrentTaskHandle()", function_body)

        for function_name, next_function in [
            ("void lwlte_handle_core_event", "void lwlte_handle_mqtt_event"),
            ("void lwlte_handle_mqtt_event", "/**********************\n *   STATIC FUNCTIONS"),
        ]:
            function_body = self.lwlte_c[
                self.lwlte_c.index(function_name):
                self.lwlte_c.index(next_function, self.lwlte_c.index(function_name))
            ]
            self.assertIn("add_callback_task_locked(me, xTaskGetCurrentTaskHandle())", function_body)
            self.assertIn("remove_callback_task_locked(me, xTaskGetCurrentTaskHandle())", function_body)
            self.assertNotIn("callback_task = xTaskGetCurrentTaskHandle()", function_body)

    def test_mqtt_null_event_data_carries_current_state_before_dispatch(self):
        post_start = self.mqtt_c.rindex("static esp_err_t post_mqtt_event")
        post_body = self.mqtt_c[
            post_start:
            self.mqtt_c.index("static void post_error_event", post_start)
        ]
        null_data_body = post_body[
            post_body.index("if (!event_data) {"):
            post_body.index("if (event_id != MQTT_CLIENT_EVENT_DATA)")
        ]

        for token in [
            "xSemaphoreTake(me->lock, portMAX_DELAY);",
            "empty_data.state = me->state;",
            "xSemaphoreGive(me->lock);",
            "event_data = &empty_data;",
        ]:
            self.assertIn(token, null_data_body)

        self.assertLess(
            null_data_body.index("xSemaphoreTake(me->lock, portMAX_DELAY);"),
            null_data_body.index("empty_data.state = me->state;")
        )
        self.assertLess(
            null_data_body.index("empty_data.state = me->state;"),
            null_data_body.index("xSemaphoreGive(me->lock);")
        )
        self.assertLess(
            null_data_body.index("xSemaphoreGive(me->lock);"),
            null_data_body.index("event_data = &empty_data;")
        )

    def test_facade_callback_task_tracking_counts_nested_same_task(self):
        self.assertIn("TaskHandle_t callback_tasks[LWLTE_CALLBACK_TASKS_MAX];", self.lwlte_priv)
        self.assertIn("int callback_task_counts[LWLTE_CALLBACK_TASKS_MAX];", self.lwlte_priv)

        add_start = self.lwlte_c.rindex("static bool add_callback_task_locked")
        add_body = self.lwlte_c[
            add_start:
            self.lwlte_c.index("static void remove_callback_task_locked", add_start)
        ]
        remove_start = self.lwlte_c.rindex("static void remove_callback_task_locked")
        remove_body = self.lwlte_c[
            remove_start:
            self.lwlte_c.index("static void wake_ready_waiters_locked", remove_start)
        ]

        for token in [
            "me->callback_tasks[i] == task",
            "me->callback_task_counts[i]++;",
            "me->callback_task_counts[i] = 1;",
            "me->callback_task_overflow++;",
        ]:
            self.assertIn(token, add_body)

        self.assertLess(
            add_body.index("me->callback_tasks[i] == task"),
            add_body.index("!me->callback_tasks[i]")
        )

        for token in [
            "me->callback_task_counts[i] > 1",
            "me->callback_task_counts[i]--;",
            "me->callback_task_counts[i] = 0;",
            "me->callback_tasks[i] = NULL;",
            "me->callback_task_overflow--",
        ]:
            self.assertIn(token, remove_body)

    def test_facade_mqtt_null_event_data_queries_current_state(self):
        event_body = self.lwlte_c[
            self.lwlte_c.index("void lwlte_handle_mqtt_event"):
            self.lwlte_c.index("/**********************\n *   STATIC FUNCTIONS")
        ]
        null_data_body = event_body[
            event_body.index("} else {"):
            event_body.index("xSemaphoreTake(me->lock", event_body.index("} else {"))
        ]

        self.assertIn("mqtt_client_state_t mqtt_state = MQTT_CLIENT_STATE_STOPPED;", null_data_body)
        self.assertIn("mqtt_client_get_state(mqtt, &mqtt_state)", null_data_body)
        self.assertIn("lwlte_data.mqtt_state = map_mqtt_state(mqtt_state);", null_data_body)
        self.assertNotIn("lwlte_data.mqtt_state = LWLTE_MQTT_STATE_STOPPED;", null_data_body)

    def test_air780ep_mqtt_config_validation_and_factory_wiring(self):
        validate_body = self.lwlte_air780ep_c[
            self.lwlte_air780ep_c.rindex("static esp_err_t validate_config"):
            self.lwlte_air780ep_c.rindex("static bool gpio_required_valid")
        ]
        for token in [
            "config->mqtt_client.enabled",
            "config->mqtt_client.host && config->mqtt_client.host[0]",
            "config->mqtt_client.port > 0",
            "config->mqtt_client.client_id && config->mqtt_client.client_id[0]",
            "non_negative_int(config->mqtt_client.fsm_queue_size)",
            "non_negative_int(config->mqtt_client.fsm_task_stack)",
            "non_negative_int(config->mqtt_client.fsm_task_priority)",
        ]:
            self.assertIn(token, validate_body)

        init_body = self.lwlte_air780ep_c[
            self.lwlte_air780ep_c.index("core_register_event_callback"):
            self.lwlte_air780ep_c.index("core_start(me->core)")
        ]
        for token in [
            "if (config->mqtt_client.enabled) {",
            "MQTT_CLIENT_TRANSPORT_PLAIN_TCP",
            "mqtt_client_create(&mqtt_config, me->core)",
            "mqtt_client_register_event_callback(me->mqtt, lwlte_handle_mqtt_event, me)",
            "cleanup_after_failure(me, ret)",
        ]:
            self.assertIn(token, init_body)

    def test_facade_mqtt_event_mapping_and_data_pointer_scope(self):
        self.assertIn("void lwlte_handle_mqtt_event", self.lwlte_c)
        event_body = self.lwlte_c[
            self.lwlte_c.index("void lwlte_handle_mqtt_event"):
            self.lwlte_c.index("/**********************\n *   STATIC FUNCTIONS")
        ]

        for token in [
            "MQTT_CLIENT_EVENT_DATA",
            "lwlte_data.data.mqtt_msg.topic = data->data.msg.topic;",
            "lwlte_data.data.mqtt_msg.topic_len = data->data.msg.topic_len;",
            "lwlte_data.data.mqtt_msg.payload = data->data.msg.payload;",
            "lwlte_data.data.mqtt_msg.payload_len = data->data.msg.payload_len;",
            "lwlte_data.mqtt_state = map_mqtt_state(data->state);",
            "lwlte_data.error_code = data->error_code;",
            "callback(me, map_mqtt_event(event_id), &lwlte_data, callback_ctx);",
        ]:
            self.assertIn(token, event_body)

        self.assertNotIn("malloc", event_body)
        self.assertNotIn("clone", event_body)

        map_body = self.lwlte_c[
            self.lwlte_c.rindex("static lwlte_event_id_t map_mqtt_event"):
            self.lwlte_c.rindex("static void map_core_event_data")
        ]
        self.assertIn("MQTT_CLIENT_EVENT_DATA", map_body)
        self.assertIn("LWLTE_EVENT_MQTT_DATA", map_body)

    def test_air780ep_mqtt_command_and_urc_ownership_contract(self):
        for token in [
            "AIR780EP_MQTT_PAYLOAD_PROMPT",
            "AT+MCONFIG=\\\"%s\\\",\\\"%s\\\",\\\"%s\\\"",
            "AT+MIPSTART=\\\"%s\\\",%u",
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


if __name__ == "__main__":
    unittest.main()
