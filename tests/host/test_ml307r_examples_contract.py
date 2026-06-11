#!/usr/bin/env python3
"""Static regression checks for ML307R examples."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
EXAMPLE_DIR = ROOT / "example"
BASIC_C = EXAMPLE_DIR / "ml307r_basic_connect.c"
MQTT_C = EXAMPLE_DIR / "ml307r_mqtt_client.c"
PROBE_C = EXAMPLE_DIR / "ml307r_probe.c"
EXAMPLE_H = EXAMPLE_DIR / "example.h"
MAIN_C = EXAMPLE_DIR / "main.c"
CMAKE = EXAMPLE_DIR / "CMakeLists.txt"
README = EXAMPLE_DIR / "README.md"
DIR_STRUCTURE = ROOT / "docs/agents/directory-structure.md"


def read_optional(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class Ml307rExamplesContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.basic_c = read_optional(BASIC_C)
        cls.mqtt_c = read_optional(MQTT_C)
        cls.example_h = read_optional(EXAMPLE_H)
        cls.main_c = read_optional(MAIN_C)
        cls.cmake = read_optional(CMAKE)
        cls.readme = read_optional(README)
        cls.dir_structure = read_optional(DIR_STRUCTURE)

    def assert_contains_all(self, text: str, tokens: list[str]):
        for token in tokens:
            self.assertIn(token, text)

    def test_probe_removed_everywhere(self):
        self.assertFalse(PROBE_C.exists(), "ml307r_probe.c should be removed")
        for label, text in [
            ("example.h", self.example_h),
            ("main.c", self.main_c),
            ("CMakeLists.txt", self.cmake),
            ("README.md", self.readme),
            ("directory-structure.md", self.dir_structure),
        ]:
            self.assertNotIn("ml307r_probe", text, label)
            self.assertNotIn("EXAMPLE_ML307R_PROBE", text, label)
            self.assertNotIn("example_ml307r_probe_run", text, label)
        self.assertNotIn("ML307R UART Probe", self.readme, "README.md")
        self.assertNotIn("raw UART probe", self.readme, "README.md")

    def test_example_selection_wiring(self):
        self.assert_contains_all(self.example_h, [
            "#define EXAMPLE_AIR780EP_BASIC_CONNECT  1",
            "#define EXAMPLE_AIR780EP_MQTT_CLIENT    2",
            "#define EXAMPLE_ML307R_BASIC_CONNECT    3",
            "#define EXAMPLE_ML307R_MQTT_CLIENT      4",
            "void example_ml307r_basic_connect_run(void);",
            "void example_ml307r_mqtt_client_run(void);",
        ])
        self.assert_contains_all(self.main_c, [
            "#define EXAMPLE_SELECTED    EXAMPLE_AIR780EP_BASIC_CONNECT",
            "case EXAMPLE_ML307R_BASIC_CONNECT:",
            "example_ml307r_basic_connect_run();",
            "case EXAMPLE_ML307R_MQTT_CLIENT:",
            "example_ml307r_mqtt_client_run();",
        ])
        self.assert_contains_all(self.cmake, [
            '"ml307r_basic_connect.c"',
            '"ml307r_mqtt_client.c"',
        ])

    def test_ml307r_basic_connect_example(self):
        self.assertTrue(BASIC_C.exists(), "missing ml307r_basic_connect.c")
        self.assert_contains_all(self.basic_c, [
            "@file ml307r_basic_connect.c",
            "ML307R LTE 基础连接示例",
            "#define TAG                                  \"ml307r_basic\"",
            "void example_ml307r_basic_connect_run(void)",
            "lwlte_ml307r_config_t",
            "lwlte_ml307r_init(&config, &lte)",
            "ML307R basic connect example",
            "ML307R network is online",
            "EXAMPLE_PING_HOST                    \"8.8.8.8\"",
            "lwlte_ping(lte, &req, replies, req.count, &summary)",
        ])
        self.assertNotIn("lwlte_air780ep_init", self.basic_c)
        self.assertNotIn("Air780EP", self.basic_c)

    def test_ml307r_mqtt_client_example(self):
        self.assertTrue(MQTT_C.exists(), "missing ml307r_mqtt_client.c")
        self.assert_contains_all(self.mqtt_c, [
            "@file ml307r_mqtt_client.c",
            "ML307R LTE MQTT 客户端示例",
            "#define TAG                                      \"ml307r_mqtt\"",
            "void example_ml307r_mqtt_client_run(void)",
            "lwlte_ml307r_config_t",
            "lwlte_ml307r_init(&config, &lte)",
            "ML307R MQTT client example",
            "TB_TOPIC_TELEMETRY                       \"v1/devices/me/telemetry\"",
            "TB_TOPIC_ATTRIBUTES                      \"v1/devices/me/attributes\"",
            "lwlte_mqtt_start(lte)",
            "lwlte_mqtt_subscribe(lte, TB_TOPIC_ATTRIBUTES, 0)",
            "lwlte_mqtt_publish(lte, TB_TOPIC_TELEMETRY,",
            "MQTT RX topic=%.*s payload=%.*s",
        ])
        self.assertNotIn("lwlte_air780ep_init", self.mqtt_c)
        self.assertNotIn("Air780EP", self.mqtt_c)

    def test_docs_describe_ml307r_examples(self):
        self.assert_contains_all(self.readme, [
            "`EXAMPLE_ML307R_BASIC_CONNECT` | ML307R LTE basic connect and ping example",
            "`EXAMPLE_ML307R_MQTT_CLIENT` | ML307R ThingsBoard MQTT publish/subscribe example",
            "## ML307R Wiring",
            "## ML307R Basic Connect",
            "## ML307R MQTT Client",
        ])
        self.assert_contains_all(self.dir_structure, [
            "ml307r_basic_connect.c",
            "ml307r_mqtt_client.c",
        ])


if __name__ == "__main__":
    unittest.main()
