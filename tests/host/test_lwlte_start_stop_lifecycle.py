#!/usr/bin/env python3
"""Static contract checks for the lwlte start/stop power-cycle lifecycle."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]

MODEM_H = ROOT / "src/modem/modem.h"
MODEM_PRIV_H = ROOT / "src/modem/modem_priv.h"
MODEM_C = ROOT / "src/modem/modem.c"
AIR780EP_C = ROOT / "src/modem/modem_air780ep.c"
ML307R_C = ROOT / "src/modem/modem_ml307r.c"
CORE_H = ROOT / "src/core/core.h"
CORE_PRIV_H = ROOT / "src/core/core_priv.h"
CORE_C = ROOT / "src/core/core.c"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"
NET_MGR_C = ROOT / "src/core/net_mgr.c"
LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"


def function_body(source: str, signature: str) -> str:
    # 跳过前置声明（`... );`），只在真正的定义（签名后先遇到 `{`）处展开。
    search_from = 0
    while True:
        start = source.find(signature, search_from)
        if start < 0:
            raise AssertionError(f"missing function definition: {signature}")
        after = start + len(signature)
        brace = source.find("{", after)
        semicolon = source.find(";", after)
        if brace >= 0 and (semicolon < 0 or brace < semicolon):
            break
        search_from = after
    depth = 0
    for idx in range(brace, len(source)):
        if source[idx] == "{":
            depth += 1
        elif source[idx] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:idx]
    raise AssertionError(f"function body not closed for {signature}")


def contains(tc, hay, needle, label):
    if needle not in hay:
        tc.fail(f"missing {needle!r} in {label}")


def absent(tc, hay, needle, label):
    if needle in hay:
        tc.fail(f"unexpected {needle!r} in {label}")


def without_c_comments(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)


class LwlteStartStopContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.modem_h = MODEM_H.read_text(encoding="utf-8")
        cls.modem_priv_h = MODEM_PRIV_H.read_text(encoding="utf-8")
        cls.modem_c = MODEM_C.read_text(encoding="utf-8")
        cls.air780ep_c = AIR780EP_C.read_text(encoding="utf-8")
        cls.ml307r_c = ML307R_C.read_text(encoding="utf-8")
        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_priv_h = CORE_PRIV_H.read_text(encoding="utf-8")
        cls.core_c = CORE_C.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")
        cls.net_mgr_c = NET_MGR_C.read_text(encoding="utf-8")
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")

    # ---- Task 1: modem base ----
    def test_modem_has_off_state(self):
        contains(self, self.modem_h, "MODEM_STATE_OFF", "modem.h")

    def test_modem_ops_has_stop(self):
        contains(self, self.modem_priv_h, "modem_no_arg_fn stop;", "modem_priv.h")

    def test_modem_stop_prototype_and_impl(self):
        contains(self, self.modem_h, "esp_err_t modem_stop(modem_handle_t *me);", "modem.h")
        body = function_body(self.modem_c, "esp_err_t modem_stop(modem_handle_t *me)")
        contains(self, body, "me->ops->stop", "modem_stop")

    def test_check_ready_allows_off(self):
        body = function_body(self.modem_c, "static esp_err_t check_ready(modem_handle_t *me, bool allow_created)")
        contains(self, body, "MODEM_STATE_OFF", "check_ready")

    # ---- Task 2: air780ep ----
    def test_air780ep_power_off_helper(self):
        body = function_body(self.air780ep_c, "static esp_err_t hardware_power_off(modem_air780ep_t *self)")
        contains(self, body, "gpio_set_level(self->config.en_pin, 0)", "air780ep hardware_power_off")
        absent(self, body, "gpio_set_level(self->config.en_pin, 1)", "air780ep hardware_power_off")

    def test_air780ep_stop_impl(self):
        body = function_body(self.air780ep_c, "static esp_err_t air780ep_stop(modem_handle_t *me)")
        for needle in ["hardware_power_off(self)", "unregister_urcs(self)", "MODEM_STATE_OFF"]:
            contains(self, body, needle, "air780ep_stop")

    def test_air780ep_stop_returns_first_cleanup_error(self):
        body = without_c_comments(function_body(self.air780ep_c, "static esp_err_t air780ep_stop(modem_handle_t *me)"))
        contains(self, body, "esp_err_t ret = ESP_OK;", "air780ep_stop")
        contains(self, body, "esp_err_t urc_ret = unregister_urcs(self);", "air780ep_stop")
        self.assertRegex(body, r"if\s*\(\s*ret\s*==\s*ESP_OK\s*\)\s*{\s*ret\s*=\s*urc_ret\s*;")
        contains(self, body, "esp_err_t power_ret = hardware_power_off(self);", "air780ep_stop")
        self.assertRegex(body, r"if\s*\(\s*ret\s*==\s*ESP_OK\s*\)\s*{\s*ret\s*=\s*power_ret\s*;")

    def test_air780ep_ops_wires_stop(self):
        contains(self, self.air780ep_c, ".stop = air780ep_stop,", "modem_air780ep.c ops")

    # ---- Task 3: ml307r ----
    def test_ml307r_power_off_helper(self):
        body = function_body(self.ml307r_c, "static esp_err_t hardware_power_off(modem_ml307r_t *self)")
        contains(self, body, "gpio_set_level(self->config.en_pin, 0)", "ml307r hardware_power_off")
        absent(self, body, "gpio_set_level(self->config.en_pin, 1)", "ml307r hardware_power_off")

    def test_ml307r_stop_impl(self):
        body = function_body(self.ml307r_c, "static esp_err_t ml307r_stop(modem_handle_t *me)")
        for needle in ["hardware_power_off(self)", "unregister_urcs(self)", "MODEM_STATE_OFF"]:
            contains(self, body, needle, "ml307r_stop")

    def test_ml307r_stop_returns_first_cleanup_error(self):
        body = without_c_comments(function_body(self.ml307r_c, "static esp_err_t ml307r_stop(modem_handle_t *me)"))
        contains(self, body, "esp_err_t ret = ESP_OK;", "ml307r_stop")
        contains(self, body, "esp_err_t urc_ret = unregister_urcs(self);", "ml307r_stop")
        self.assertRegex(body, r"if\s*\(\s*ret\s*==\s*ESP_OK\s*\)\s*{\s*ret\s*=\s*urc_ret\s*;")
        contains(self, body, "esp_err_t power_ret = hardware_power_off(self);", "ml307r_stop")
        self.assertRegex(body, r"if\s*\(\s*ret\s*==\s*ESP_OK\s*\)\s*{\s*ret\s*=\s*power_ret\s*;")

    def test_ml307r_ops_wires_stop(self):
        contains(self, self.ml307r_c, ".stop = ml307r_stop,", "modem_ml307r.c ops")

    # ---- Task 4: core stop_pending ----
    def test_core_handle_has_stop_pending(self):
        contains(self, self.core_priv_h, "stop_pending", "core_priv.h")

    def test_core_stop_sets_pending(self):
        body = function_body(self.core_c, "esp_err_t core_stop(core_handle_t *me)")
        contains(self, body, "stop_pending = true", "core_stop")

    def test_core_stop_sets_pending_after_signal_submission(self):
        body = without_c_comments(function_body(self.core_c, "esp_err_t core_stop(core_handle_t *me)"))
        contains(self, body, "xQueueSend(me->fsm.queue, &sig, 0)", "core_stop")
        contains(self, body, "BaseType_t send_ret", "core_stop")
        self.assertLess(body.index("xQueueSend(me->fsm.queue, &sig, 0)"),
                        body.index("stop_pending = true"))
        self.assertRegex(body,
                         r"if\s*\(\s*send_ret\s*==\s*pdTRUE\s*\)\s*{[^}]*stop_pending\s*=\s*true\s*;",
                         "stop_pending must only be set after successful queue send")

    def test_core_start_marks_starting_after_signal_submission(self):
        body = without_c_comments(function_body(self.core_c, "esp_err_t core_start(core_handle_t *me)"))
        contains(self, body, "xQueueSend(me->fsm.queue, &sig, 0)", "core_start")
        contains(self, body, "me->state = CORE_STATE_STARTING", "core_start")
        self.assertLess(body.index("xQueueSend(me->fsm.queue, &sig, 0)"),
                        body.index("me->state = CORE_STATE_STARTING"))
        self.assertRegex(body,
                         r"if\s*\(\s*send_ret\s*==\s*pdTRUE\s*\)\s*{[^}]*me->state\s*=\s*CORE_STATE_STARTING\s*;[^}]*stop_pending\s*=\s*false\s*;",
                         "queued start must transition to STARTING and clear stale stop_pending atomically")

    def test_core_start_rechecks_stopped_under_lock(self):
        body = without_c_comments(function_body(self.core_c, "esp_err_t core_start(core_handle_t *me)"))
        locked = body[body.index("xSemaphoreTake(me->lock"):
                      body.index("BaseType_t send_ret")]
        contains(self, locked, "me->state != CORE_STATE_STOPPED", "core_start locked state recheck")

    def test_net_mgr_cooperative_cancel(self):
        body = function_body(self.net_mgr_c, "static esp_err_t check_activation_continue(core_handle_t *me,")
        contains(self, body, "core_stop_pending(me)", "check_activation_continue")

    def test_handle_stop_clears_stop_pending(self):
        body = function_body(self.core_fsm_c, "static void handle_stop(core_handle_t *me)")
        contains(self, body, "stop_pending = false", "handle_stop")

    # ---- Task 5: handle_stop power-off + service_cmd guard ----
    def test_handle_stop_powers_off_modem(self):
        body = function_body(self.core_fsm_c, "static void handle_stop(core_handle_t *me)")
        contains(self, body, "modem_stop(me->modem)", "handle_stop")
        contains(self, body, "stop_pending = false", "handle_stop")

    def test_handle_stop_powers_off_after_deactivate_before_stopped(self):
        body = without_c_comments(function_body(self.core_fsm_c, "static void handle_stop(core_handle_t *me)"))
        contains(self, body, "net_mgr_deactivate(me)", "handle_stop")
        contains(self, body, "modem_stop(me->modem)", "handle_stop")
        contains(self, body, "core_set_state(me, CORE_STATE_STOPPED)", "handle_stop")
        self.assertLess(body.index("net_mgr_deactivate(me)"),
                        body.index("modem_stop(me->modem)"))
        self.assertLess(body.index("modem_stop(me->modem)"),
                        body.index("core_set_state(me, CORE_STATE_STOPPED)"))

    def test_handle_start_respects_stop_pending(self):
        body = function_body(self.core_fsm_c, "static void handle_start(core_handle_t *me)")
        contains(self, body, "core_stop_pending(me)", "handle_start")
        body_no_comments = without_c_comments(body)
        first_pending = body_no_comments.index("core_stop_pending(me)")
        modem_start = body_no_comments.index("modem_start(me->modem)")
        second_pending = body_no_comments.index("core_stop_pending(me)", modem_start)
        handle_ready = body_no_comments.index("handle_ready(me)")
        self.assertLess(first_pending, modem_start)
        self.assertLess(modem_start, second_pending)
        self.assertLess(second_pending, handle_ready)

    def test_service_cmd_guarded_when_stopped(self):
        body = without_c_comments(function_body(self.core_fsm_c, "static void handle_service_cmd(core_handle_t *me, core_cmd_t *cmd)"))
        contains(self, body, "CORE_STATE_STOPPED", "handle_service_cmd")
        contains(self, body, "ESP_ERR_INVALID_STATE", "handle_service_cmd")
        contains(self, body, "&invalid_state", "handle_service_cmd")

    # ---- Task 6: disconnect removed ----
    def test_core_disconnect_removed(self):
        absent(self, self.core_h, "core_disconnect", "core.h")
        absent(self, self.core_c, "esp_err_t core_disconnect", "core.c")
        absent(self, self.core_priv_h, "CORE_SIG_NET_DEACTIVATE", "core_priv.h")

    # ---- Task 7: facade stop ----
    def test_facade_has_stop_not_disconnect(self):
        contains(self, self.lwlte_h, "esp_err_t lwlte_stop(lwlte_handle_t *me);", "lwlte.h")
        absent(self, self.lwlte_h, "lwlte_disconnect", "lwlte.h")
        absent(self, self.lwlte_c, "lwlte_disconnect", "lwlte.c")

    def test_lwlte_stop_impl(self):
        body = function_body(self.lwlte_c, "esp_err_t lwlte_stop(lwlte_handle_t *me)")
        contains(self, body, "core_stop(core)", "lwlte_stop")
        contains(self, body, "mqtt_client_stop", "lwlte_stop")
        contains(self, body, "esp_err_t mqtt_ret = ESP_OK", "lwlte_stop")
        contains(self, body, "mqtt_client_stop(mqtt)", "lwlte_stop")
        contains(self, body, "xSemaphoreGive(me->lock)", "lwlte_stop")
        self.assertLess(body.index("mqtt_client_stop(mqtt)"),
                        body.index("xSemaphoreGive(me->lock)"))
        contains(self, body, "return ret;", "lwlte_stop")


if __name__ == "__main__":
    unittest.main()
