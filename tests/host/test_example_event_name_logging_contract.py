#!/usr/bin/env python3
"""Static checks for human-readable example event logs."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = ROOT / "example"
HELPER_H = EXAMPLE_DIR / "example_event_names.h"
HELPER_C = EXAMPLE_DIR / "example_event_names.c"
CMAKE = EXAMPLE_DIR / "CMakeLists.txt"

EXAMPLE_FILES = [
    EXAMPLE_DIR / "air780ep_basic_connect.c",
    EXAMPLE_DIR / "air780ep_mqtt_client.c",
    EXAMPLE_DIR / "air780ep_tcp_client.c",
    EXAMPLE_DIR / "ml307r_basic_connect.c",
    EXAMPLE_DIR / "ml307r_mqtt_client.c",
    EXAMPLE_DIR / "ml307r_tcp_client.c",
]


def read_optional(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if not match:
        return ""
    start = match.end() - 1
    depth = 0
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    return ""


class ExampleEventNameLoggingContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper_h = read_optional(HELPER_H)
        cls.helper_c = read_optional(HELPER_C)
        cls.cmake = read_optional(CMAKE)
        cls.examples = {path.name: read_optional(path) for path in EXAMPLE_FILES}

    def assert_contains_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def assert_case_returns(self, body: str, case_token: str, name: str):
        pattern = rf"case\s+{re.escape(case_token)}\s*:\s*return\s+\"{re.escape(name)}\"\s*;"
        self.assertRegex(body, pattern, f"{case_token} should return {name}")

    def test_helper_header_declares_name_functions(self):
        self.assertTrue(HELPER_H.exists(), "missing example_event_names.h")
        self.assert_contains_all(self.helper_h, [
            "#include \"lwlte.h\"",
            "const char *example_lwlte_event_name(lwlte_event_id_t id);",
            "const char *example_lwlte_net_state_name(lwlte_net_state_t state);",
            "const char *example_lwlte_mqtt_event_name(lwlte_mqtt_event_id_t id);",
            "const char *example_lwlte_tcp_event_name(lwlte_tcp_event_id_t id);",
            "const char *example_lwlte_tcp_conn_state_name(lwlte_tcp_conn_state_t state);",
        ], "example_event_names.h")

    def test_helper_source_maps_public_event_and_state_values(self):
        self.assertTrue(HELPER_C.exists(), "missing example_event_names.c")
        bodies = {
            "example_lwlte_event_name": function_body(self.helper_c, "example_lwlte_event_name"),
            "example_lwlte_net_state_name": function_body(self.helper_c, "example_lwlte_net_state_name"),
            "example_lwlte_mqtt_event_name": function_body(self.helper_c, "example_lwlte_mqtt_event_name"),
            "example_lwlte_tcp_event_name": function_body(self.helper_c, "example_lwlte_tcp_event_name"),
            "example_lwlte_tcp_conn_state_name": function_body(self.helper_c, "example_lwlte_tcp_conn_state_name"),
        }
        for helper_name, body in bodies.items():
            self.assertTrue(body, f"missing function {helper_name}")
            self.assertIn('return "UNKNOWN";', body, helper_name)

        for case_token, name in [
            ("LWLTE_EVENT_STARTED", "STARTED"),
            ("LWLTE_EVENT_READY", "READY"),
            ("LWLTE_EVENT_NET_CONNECTING", "NET_CONNECTING"),
            ("LWLTE_EVENT_NET_ONLINE", "NET_ONLINE"),
            ("LWLTE_EVENT_NET_OFFLINE", "NET_OFFLINE"),
            ("LWLTE_EVENT_NET_ERROR", "NET_ERROR"),
            ("LWLTE_EVENT_STOPPED", "STOPPED"),
            ("LWLTE_EVENT_ERROR", "ERROR"),
        ]:
            self.assert_case_returns(bodies["example_lwlte_event_name"], case_token, name)

        for case_token, name in [
            ("LWLTE_NET_STATE_OFFLINE", "OFFLINE"),
            ("LWLTE_NET_STATE_ACTIVATING", "ACTIVATING"),
            ("LWLTE_NET_STATE_ONLINE", "ONLINE"),
            ("LWLTE_NET_STATE_ERROR", "ERROR"),
        ]:
            self.assert_case_returns(bodies["example_lwlte_net_state_name"], case_token, name)

        for case_token, name in [
            ("LWLTE_MQTT_EVENT_STARTED", "STARTED"),
            ("LWLTE_MQTT_EVENT_STOPPED", "STOPPED"),
            ("LWLTE_MQTT_EVENT_CONNECTING", "CONNECTING"),
            ("LWLTE_MQTT_EVENT_CONNECTED", "CONNECTED"),
            ("LWLTE_MQTT_EVENT_DISCONNECTED", "DISCONNECTED"),
            ("LWLTE_MQTT_EVENT_SUBSCRIBED", "SUBSCRIBED"),
            ("LWLTE_MQTT_EVENT_UNSUBSCRIBED", "UNSUBSCRIBED"),
            ("LWLTE_MQTT_EVENT_PUBLISHED", "PUBLISHED"),
            ("LWLTE_MQTT_EVENT_DATA", "DATA"),
            ("LWLTE_MQTT_EVENT_ERROR", "ERROR"),
        ]:
            self.assert_case_returns(bodies["example_lwlte_mqtt_event_name"], case_token, name)

        for case_token, name in [
            ("LWLTE_TCP_EVENT_STARTED", "STARTED"),
            ("LWLTE_TCP_EVENT_STOPPED", "STOPPED"),
            ("LWLTE_TCP_EVENT_CONNECTED", "CONNECTED"),
            ("LWLTE_TCP_EVENT_DISCONNECTED", "DISCONNECTED"),
            ("LWLTE_TCP_EVENT_SENT", "SENT"),
            ("LWLTE_TCP_EVENT_DATA", "DATA"),
            ("LWLTE_TCP_EVENT_ERROR", "ERROR"),
        ]:
            self.assert_case_returns(bodies["example_lwlte_tcp_event_name"], case_token, name)

        for case_token, name in [
            ("LWLTE_TCP_CONN_STATE_CREATED", "CREATED"),
            ("LWLTE_TCP_CONN_STATE_CONNECTING", "CONNECTING"),
            ("LWLTE_TCP_CONN_STATE_CONNECTED", "CONNECTED"),
            ("LWLTE_TCP_CONN_STATE_CLOSING", "CLOSING"),
            ("LWLTE_TCP_CONN_STATE_CLOSED", "CLOSED"),
            ("LWLTE_TCP_CONN_STATE_ERROR", "ERROR"),
        ]:
            self.assert_case_returns(bodies["example_lwlte_tcp_conn_state_name"], case_token, name)

    def test_cmake_builds_helper_source(self):
        self.assertIn('"example_event_names.c"', self.cmake)

    def test_lte_event_logs_include_event_and_net_state_names(self):
        for name in [
            "air780ep_basic_connect.c",
            "ml307r_basic_connect.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"LTE event=%d(%s) net=%d(%s) err=%d"', source, name)
            self.assertIn("example_lwlte_event_name(", source, name)
            self.assertIn("example_lwlte_net_state_name(", source, name)
            self.assertNotIn('"LTE event=%d net=%d err=%d"', source, name)

    def test_mqtt_examples_include_lte_and_mqtt_event_names(self):
        for name in [
            "air780ep_mqtt_client.c",
            "ml307r_mqtt_client.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"LTE event=%d(%s)"', source, name)
            self.assertIn("example_lwlte_event_name(", source, name)
            self.assertIn('"MQTT event=%d(%s)"', source, name)
            self.assertIn("example_lwlte_mqtt_event_name(", source, name)
            self.assertNotIn('"LTE event=%d"', source, name)
            self.assertNotIn('"MQTT event=%d"', source, name)

    def test_tcp_examples_include_tcp_event_and_state_names(self):
        for name in [
            "air780ep_tcp_client.c",
            "ml307r_tcp_client.c",
        ]:
            source = self.examples[name]
            self.assertIn('#include "example_event_names.h"', source, name)
            self.assertIn('"TCP event=%d(%s) state=%d(%s) err=%d modem=%d reason=%d"', source, name)
            self.assertIn("example_lwlte_tcp_event_name(", source, name)
            self.assertIn("example_lwlte_tcp_conn_state_name(", source, name)
            self.assertNotIn('"TCP event=%d state=%d err=%d modem=%d reason=%d"', source, name)


if __name__ == "__main__":
    unittest.main()
