#!/usr/bin/env python3
"""Static regression checks for the Ping Service class documentation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
CLASSES_MD = ROOT / "docs/agents/classes.md"


class PingClassesDocContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.classes_md = CLASSES_MD.read_text(encoding="utf-8")

    def test_visibility_table_mentions_ping_service(self):
        for token in [
            "src/ping_client/ping_client.h",
            "ping_client_",
            "Ping Service",
            "AT Engine、Modem、Core、MQTT Client Service 和 Ping Service 都没有任何用户 API",
        ]:
            self.assertIn(token, self.classes_md)

    def test_modem_ping_contract_is_documented(self):
        for token in [
            "modem_ping_request_t",
            "modem_ping_reply_t",
            "modem_ping_summary_t",
            "esp_err_t modem_ping(modem_t *me,",
            "esp_err_t (*ping)(modem_t *me,",
            "| `ping` | 执行网络连通性诊断，不参与 Core online 条件 | `AT+CIPPING` |",
            "AT+CIPPING` 现在作为 `modem_ping()` 的 Air780EP 映射",
        ]:
            self.assertIn(token, self.classes_md)

    def test_core_ping_command_contract_is_documented(self):
        for token in [
            "core_ping_reply_t",
            "core_ping_summary_t",
            "CORE_CMD_PING",
            "core_ping_reply_t *replies;",
            "core_ping_summary_t *summary;",
            "`CORE_CMD_PING` 的 `host` 由 `core_submit_cmd()` 深拷贝",
            "`replies` 和 `summary` 是同步 Ping Service 调用持有的输出 buffer",
            "Core 网络未 online 时，`CORE_CMD_PING` 返回 `CORE_CMD_RESULT_ERROR`",
        ]:
            self.assertIn(token, self.classes_md)

    def test_ping_service_section_is_after_mqtt_before_app(self):
        mqtt_idx = self.classes_md.find("## 4. MQTT Client Service")
        ping_idx = self.classes_md.find("## 5. Ping Service")
        app_idx = self.classes_md.find("## 6. App")
        self.assertNotEqual(-1, mqtt_idx)
        self.assertNotEqual(-1, ping_idx)
        self.assertNotEqual(-1, app_idx)
        self.assertLess(mqtt_idx, ping_idx)
        self.assertLess(ping_idx, app_idx)

    def test_ping_service_is_lightweight_and_boundary_safe(self):
        for token in [
            "Ping Service 不创建自己的 FSM task、FSM queue 或 esp_event loop",
            "ping_client_create(core_t *core);",
            "ping_client_ping(ping_client_t *me,",
            "core_submit_cmd(CORE_CMD_PING)",
            "Ping Service 不 include `modem.h`、`modem_air780ep.h`、`at_engine.h`",
            "不能把 `lwlte_ping_reply_t *` 强转成 `core_ping_reply_t *`",
        ]:
            self.assertIn(token, self.classes_md)

    def test_public_facade_ping_api_is_documented(self):
        for token in [
            "lwlte_ping_request_t",
            "lwlte_ping_reply_t",
            "lwlte_ping_summary_t",
            "esp_err_t lwlte_ping(lwlte_t *me,",
            "调用方负责传入 `lwlte_ping_reply_t` 数组",
            "第一版只实现同步阻塞 `lwlte_ping()`",
            "`lwlte_ping_async()`",
        ]:
            self.assertIn(token, self.classes_md)


if __name__ == "__main__":
    unittest.main()
