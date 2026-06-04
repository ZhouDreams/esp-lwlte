#!/usr/bin/env python3
"""Static regression checks for the lwlte init/start lifecycle split."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]

LWLTE_H = ROOT / "src/include/lwlte.h"
LWLTE_C = ROOT / "src/lwlte/lwlte.c"
LWLTE_AIR780EP_C = ROOT / "src/lwlte/lwlte_air780ep.c"
CORE_H = ROOT / "src/core/core.h"
CORE_FSM_C = ROOT / "src/core/core_fsm.c"
ARCH_DOC = ROOT / "docs/agents/architecture.md"
CLASSES_DOC = ROOT / "docs/agents/classes.md"
OOP_DOC = ROOT / "docs/agents/oop-design.md"
BASIC_EXAMPLE = ROOT / "example/basic_connect.c"
MQTT_EXAMPLE = ROOT / "example/mqtt_client.c"


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


def assert_contains(testcase: unittest.TestCase, haystack: str, needle: str, label: str) -> None:
    if needle not in haystack:
        testcase.fail(f"missing {needle!r} in {label}")


def assert_not_contains(testcase: unittest.TestCase, haystack: str, needle: str, label: str) -> None:
    if needle in haystack:
        testcase.fail(f"unexpected {needle!r} in {label}")


def require_index(testcase: unittest.TestCase, haystack: str, needle: str, label: str, start: int = 0) -> int:
    index = haystack.find(needle, start)
    if index < 0:
        testcase.fail(f"missing anchor {needle!r} in {label}")
    return index


class LwlteStartLifecycleContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = LWLTE_H.read_text(encoding="utf-8")
        cls.lwlte_c = LWLTE_C.read_text(encoding="utf-8")
        cls.lwlte_air780ep_c = LWLTE_AIR780EP_C.read_text(encoding="utf-8")
        cls.core_h = CORE_H.read_text(encoding="utf-8")
        cls.core_fsm_c = CORE_FSM_C.read_text(encoding="utf-8")
        cls.arch_doc = ARCH_DOC.read_text(encoding="utf-8")
        cls.classes_doc = CLASSES_DOC.read_text(encoding="utf-8")
        cls.oop_doc = OOP_DOC.read_text(encoding="utf-8")
        cls.basic_example = read_optional(BASIC_EXAMPLE)
        cls.mqtt_example = read_optional(MQTT_EXAMPLE)

    def test_public_api_has_start_not_connect_or_auto_connect(self):
        assert_contains(self, self.lwlte_h, "esp_err_t lwlte_start(lwlte_t *me);", "lwlte.h")
        assert_not_contains(self, self.lwlte_h, "esp_err_t lwlte_connect(lwlte_t *me);", "lwlte.h")
        config_start = require_index(self, self.lwlte_h, "typedef struct {", "lwlte.h")
        config_start = require_index(self, self.lwlte_h, "uart_port_t uart_num;", "lwlte_air780ep_config_t", config_start)
        config_end = require_index(self, self.lwlte_h, "} lwlte_air780ep_config_t;", "lwlte_air780ep_config_t", config_start)
        config_body = self.lwlte_h[config_start:config_end]
        assert_not_contains(self, config_body, "auto_connect", "lwlte_air780ep_config_t")

    def test_facade_start_delegates_only_to_core_start(self):
        body = function_body(self.lwlte_c, "esp_err_t lwlte_start(lwlte_t *me)")
        assert_contains(self, body, "begin_api_call(me, true, &core)", "lwlte_start")
        assert_contains(self, body, "core_start(core)", "lwlte_start")
        for forbidden in ["modem_start", "lwlte_wait_ready", "core_connect", "modem_"]:
            assert_not_contains(self, body, forbidden, "lwlte_start")

    def test_air780ep_init_constructs_without_runtime_start(self):
        body = function_body(
            self.lwlte_air780ep_c,
            "esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,",
        )
        for required in ["at_engine_create", "modem_air780ep_create", "core_create", "core_register_event_callback"]:
            assert_contains(self, body, required, "lwlte_air780ep_init")
        for forbidden in ["modem_start", "core_start", "lwlte_wait_ready", "lwlte_connect", "auto_connect"]:
            assert_not_contains(self, body, forbidden, "lwlte_air780ep_init")

    def test_core_start_owns_modem_start_and_network_activation(self):
        assert_not_contains(self, self.core_h, "bool auto_connect;", "core.h")
        handle_start = function_body(self.core_fsm_c, "static void handle_start(core_t *me)")
        for required in [
            "core_set_state(me, CORE_STATE_STARTING)",
            "post_event_checked(me, CORE_EVENT_STARTED, NULL)",
            "modem_start(me->modem)",
            "handle_ready(me)",
            "net_mgr_start_activation(me)",
        ]:
            assert_contains(self, handle_start, required, "handle_start")
        self.assertLess(handle_start.index("modem_start(me->modem)"), handle_start.index("net_mgr_start_activation(me)"))

    def test_core_ready_no_longer_auto_connects(self):
        handle_ready = function_body(self.core_fsm_c, "static void handle_ready(core_t *me)")
        assert_contains(self, handle_ready, "core_set_state(me, CORE_STATE_READY)", "handle_ready")
        assert_contains(self, handle_ready, "post_event_checked(me, CORE_EVENT_READY, NULL)", "handle_ready")
        assert_not_contains(self, handle_ready, "auto_connect", "handle_ready")
        assert_not_contains(self, handle_ready, "net_mgr_start_activation", "handle_ready")

    def test_examples_use_explicit_start(self):
        failures = []
        for label, source in [
            ("example/basic_connect.c", self.basic_example),
            ("example/mqtt_client.c", self.mqtt_example),
        ]:
            if "lwlte_start" not in source:
                failures.append(f"missing 'lwlte_start' in {label}")
            if ".auto_connect" in source:
                failures.append(f"unexpected '.auto_connect' in {label}")
            if "lwlte_connect" in source:
                failures.append(f"unexpected 'lwlte_connect' in {label}")

        if failures:
            self.fail("; ".join(failures))

    def test_docs_describe_new_lifecycle(self):
        docs = self.arch_doc + self.classes_doc + self.oop_doc
        for token in [
            "lwlte_start",
            "LWLTE_EVENT_NET_ONLINE",
            "modem_start",
            "PDP",
        ]:
            assert_contains(self, docs, token, "docs")
        forbidden_patterns = [
            r"auto_connect\s*=\s*true",
            r"auto_connect\s*=\s*false",
        ]
        for pattern in forbidden_patterns:
            self.assertIsNone(re.search(pattern, docs, re.DOTALL), pattern)
        for old_sequence in [
            "if (modem_start(modem) != ESP_OK)",
            "if (core_start(core) != ESP_OK)",
            "lwlte_wait_ready(lte",
            "if (config->auto_connect",
        ]:
            assert_not_contains(self, docs, old_sequence, "docs")


if __name__ == "__main__":
    unittest.main()
