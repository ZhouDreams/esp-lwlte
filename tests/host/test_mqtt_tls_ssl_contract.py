#!/usr/bin/env python3
"""Static contract checks for MQTT TLS and reusable SSL provisioning."""

import re
import shutil
import subprocess
import tempfile
import textwrap
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    return (ROOT / rel_path).read_text(encoding="utf-8")


class MqttTlsSslContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.modem_priv_h = read("src/modem/modem_priv.h")
        cls.modem_c = read("src/modem/modem.c")
        cls.mqtt_h = read("src/mqtt_client/mqtt_client.h")
        cls.mqtt_priv_h = read("src/mqtt_client/mqtt_client_priv.h")
        cls.mqtt_c = read("src/mqtt_client/mqtt_client.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")
        cls.example_kconfig = read("example/Kconfig.projbuild")
        cls.air_example = read("example/air780ep_mqtt_client.c")
        cls.ml_example = read("example/ml307r_mqtt_client.c")
        cls.roadmap = read("docs/agents/feature-roadmap.md")

    def assert_has_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def _example_tls_block(self, text: str, label: str) -> str:
        start = text.find("if (CONFIG_EXAMPLE_MQTT_TLS_ENABLE) {")
        self.assertNotEqual(start, -1, f"{label} missing TLS enable block")
        end = text.find("ret = lwlte_mqtt_start(lte);", start)
        self.assertNotEqual(end, -1, f"{label} missing MQTT start after TLS block")
        return text[start:end]

    def _example_pem_normalizer(self, text: str, label: str) -> str:
        start = text.rfind("static char *example_normalize_pem_newlines")
        self.assertNotEqual(start, -1, f"{label} missing PEM normalizer")
        end = text.find("\n\nstatic void idle_forever", start)
        self.assertNotEqual(end, -1, f"{label} missing PEM normalizer end marker")
        return text[start:end]

    def test_public_ssl_api_and_mqtt_transport_exist(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_SSL_AUTH_NONE",
            "LWLTE_SSL_AUTH_SERVER",
            "LWLTE_SSL_AUTH_MUTUAL",
            "lwlte_ssl_context_config_t",
            "lwlte_ssl_credentials_t",
            "lwlte_ssl_context_status_t",
            "bool provisioned;",
            "bool ca_cert_present;",
            "bool client_cert_present;",
            "bool client_key_present;",
            "bool check_valid;",
            "esp_err_t lwlte_ssl_provision(lwlte_handle_t *me,",
            "esp_err_t lwlte_ssl_get_context_status(lwlte_handle_t *me,",
            "LWLTE_MQTT_TRANSPORT_PLAIN_TCP",
            "LWLTE_MQTT_TRANSPORT_TLS",
            "lwlte_mqtt_transport_t transport;",
            "uint8_t ssl_context_id;",
        ], "lwlte.h")

    def test_core_and_modem_have_ssl_command_path(self):
        self.assert_has_all(self.core_h + self.core_c + self.core_fsm_c, [
            "CORE_CMD_SSL_PROVISION",
            "CORE_CMD_SSL_GET_CONTEXT_STATUS",
            "return type >= CORE_CMD_SSL_PROVISION",
            "core_ssl_context_config_t",
            "core_ssl_credentials_t",
            "core_ssl_context_status_t",
            "modem_ssl_provision",
            "modem_ssl_get_context_status",
            "ca_cert_pem",
            "client_key_pem",
            "ssl_cmd && ret != ESP_OK",
        ], "core SSL path")
        self.assert_has_all(self.modem_h + self.modem_priv_h + self.modem_c, [
            "modem_ssl_context_config_t",
            "modem_ssl_credentials_t",
            "modem_ssl_context_status_t",
            "modem_ssl_provision",
            "modem_ssl_get_context_status",
            "ssl_provision",
            "ssl_get_context_status",
        ], "modem SSL path")

    def test_mqtt_service_carries_tls_without_crossing_boundaries(self):
        self.assert_has_all(self.mqtt_h + self.mqtt_c, [
            "MQTT_CLIENT_TRANSPORT_TLS",
            "ssl_context_id",
            "cmd.data.mqtt_config.transport",
            "cmd.data.mqtt_config.ssl_context_id",
        ], "mqtt TLS path")
        self.assertNotIn("rejects TLS", self.mqtt_c)
        self.assertNotIn("config->endpoint.transport == MQTT_CLIENT_TRANSPORT_TLS) {\n        return NULL;", self.mqtt_c)
        mqtt_service_text = self.mqtt_c + self.mqtt_h + self.mqtt_priv_h
        for forbidden_header in [
            "modem.h",
            "modem_priv.h",
            "modem_air780ep.h",
            "modem_ml307r.h",
            "at_engine.h",
            "core_priv.h",
        ]:
            pattern = re.compile(rf'^\s*#\s*include\s*[<"](?:[^<>"]*/)?{re.escape(forbidden_header)}[>"]', re.MULTILINE)
            self.assertNotRegex(mqtt_service_text, pattern)

    def test_air780ep_ssl_mapping_exists(self):
        self.assert_has_all(self.air_c, [
            "AIR780EP_SSL_MQTT_CONTEXT_ID",
            "AIR780EP_SSL_MAX_PEM_LEN",
            "air780ep_ssl_provision",
            "air780ep_ssl_get_context_status",
            "air780ep_query_ssl_auth_mode",
            "air780ep_clear_ssl_state",
            "AT+FSCREATE=",
            "AT+FSWRITE=",
            "AT+FSFLSIZE=",
            "AT+SSLCFG=\"cacert\",88",
            "AT+SSLCFG=\"seclevel\",88",
            "AT+SSLMIPSTART=",
            ".ssl_provision = air780ep_ssl_provision",
            ".ssl_get_context_status = air780ep_ssl_get_context_status",
        ], "Air780EP SSL mapping")

    def test_air780ep_fsdel_ignores_missing_file_cme_variants(self):
        delete_fn = re.search(
            r"^static esp_err_t air780ep_delete_file_ignore_missing\(modem_air780ep_t \*self,\n"
            r"\s+const char \*name\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t air780ep_write_file",
            self.air_c,
            re.S | re.M,
        )
        self.assertIsNotNone(delete_fn, "missing air780ep_delete_file_ignore_missing body")
        body = delete_fn.group("body")
        self.assertIn("ctx.response.error_code == 62", body)
        self.assertIn("ctx.response.error_code == 100", body)

        write_fn = re.search(
            r"^static esp_err_t air780ep_write_file\(modem_air780ep_t \*self, const char \*name,\n"
            r"\s+const uint8_t \*payload, size_t len\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t air780ep_bind_ssl_file",
            self.air_c,
            re.S | re.M,
        )
        self.assertIsNotNone(write_fn, "missing air780ep_write_file body")
        self.assertLess(
            write_fn.group("body").index("air780ep_delete_file_ignore_missing"),
            write_fn.group("body").index("AT+FSCREATE="),
        )

    def test_air780ep_fsfllsize_treats_missing_cme_variants_as_absent(self):
        file_exists_fn = re.search(
            r"^static esp_err_t air780ep_file_exists\(modem_air780ep_t \*self, const char \*name,\n"
            r"\s+bool \*exists\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t air780ep_query_ssl_auth_mode",
            self.air_c,
            re.S | re.M,
        )
        self.assertIsNotNone(file_exists_fn, "missing air780ep_file_exists body")
        body = file_exists_fn.group("body")
        self.assertIn("*exists = false;", body)
        self.assertIn("ctx.response.error_code == 62", body)
        self.assertIn("ctx.response.error_code == 100", body)
        self.assertLess(
            body.index("*exists = false;"),
            body.index("ctx.response.error_code == 100"),
        )

    def test_air780ep_server_auth_uses_root_certificate_verify_mode(self):
        provision_fn = re.search(
            r"^static esp_err_t air780ep_ssl_provision\(modem_handle_t \*me,\n"
            r"\s+const modem_ssl_context_config_t \*config,\n"
            r"\s+const modem_ssl_credentials_t \*credentials\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t air780ep_ssl_get_context_status",
            self.air_c,
            re.S | re.M,
        )
        self.assertIsNotNone(provision_fn, "missing air780ep_ssl_provision body")
        body = provision_fn.group("body")
        self.assertIn("AT+SSLCFG=\\\"verifymode\\\",88,0", body)
        self.assertLess(
            body.index("AT+SSLCFG=\\\"verifymode\\\",88,0"),
            body.index("ssl_mark_context"),
        )

    def test_ml307r_ssl_mapping_exists(self):
        self.assert_has_all(self.ml_c, [
            "ML307R_SSL_MAX_PEM_LEN",
            "ml307r_ssl_provision",
            "ml307r_ssl_get_context_status",
            "ml307r_query_ssl_auth_mode",
            "ml307r_clear_ssl_state",
            "AT+MSSLCERTWR=",
            "AT+MSSLKEYWR=",
            "AT+MSSLCFG=\"auth\"",
            "AT+MSSLCFG=\"cert\"",
            "AT+MSSLCFG=\"ignoreverify\"",
            "AT+MSSLLIST=",
            "AT+MSSLCHECK=",
            "AT+MQTTCFG=\"ssl\",0,1",
            "AT+MQTTCFG=\"ssl\",0,0",
            ".ssl_provision = ml307r_ssl_provision",
            ".ssl_get_context_status = ml307r_ssl_get_context_status",
        ], "ML307R SSL mapping")

    def test_ml307r_msslrm_ignores_missing_or_absent_object_errors(self):
        remove_fn = re.search(
            r"^static esp_err_t ml307r_remove_ssl_object_ignore_missing\(modem_ml307r_t \*self,\n"
            r"\s+const char \*name\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t ml307r_write_cert_object",
            self.ml_c,
            re.S | re.M,
        )
        self.assertIsNotNone(remove_fn, "missing ml307r_remove_ssl_object_ignore_missing body")
        body = remove_fn.group("body")
        self.assertIn("ctx.response.error_code == 762", body)
        self.assertIn("ctx.response.error_code == 767", body)

        write_fn = re.search(
            r"^static esp_err_t ml307r_write_cert_object\(modem_ml307r_t \*self, const char \*name,\n"
            r"\s+const uint8_t \*data, size_t len,\n"
            r"\s+bool private_key\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t ml307r_ssl_object_present",
            self.ml_c,
            re.S | re.M,
        )
        self.assertIsNotNone(write_fn, "missing ml307r_write_cert_object body")
        write_body = write_fn.group("body")
        self.assertLess(
            write_body.index("ml307r_remove_ssl_object_ignore_missing"),
            write_body.index("AT+MSSLCERTWR="),
        )

    def test_ml307r_mutual_status_checks_present_required_objects(self):
        status_fn = re.search(
            r"static esp_err_t ml307r_ssl_get_context_status\(.*?\n}\n\nstatic char \*escape_at_string",
            self.ml_c,
            re.S,
        )
        self.assertIsNotNone(status_fn, "missing ml307r_ssl_get_context_status body")

        check_build = re.search(
            r"const char \*required_checks\[3\];(?P<section>.*?)bool checks_ok",
            status_fn.group(0),
            re.S,
        )
        self.assertIsNotNone(check_build, "missing ML307R SSL check list construction")
        section = check_build.group("section")

        self.assertNotRegex(section, r"MODEM_SSL_AUTH_MUTUAL\s*&&\s*ca_exists")
        self.assertRegex(
            section,
            r"status->auth_mode\s*==\s*MODEM_SSL_AUTH_MUTUAL\s*\)\s*{",
        )
        for exists_name, object_name in [
            ("ca_exists", "ca_name"),
            ("cert_exists", "client_cert_name"),
            ("key_exists", "client_key_name"),
        ]:
            self.assertRegex(
                section,
                rf"if\s*\(\s*{exists_name}\s*\)\s*{{\s*"
                rf"required_checks\[required_count\+\+\]\s*=\s*{object_name};\s*}}",
            )

        self.assertNotIn("checks_ok && i < required_count", status_fn.group(0))

    def test_ml307r_ssl_reprovision_rejects_active_tls_before_invalidation(self):
        provision_fn = re.search(
            r"^static esp_err_t ml307r_ssl_provision\(modem_handle_t \*me,\n"
            r"\s+const modem_ssl_context_config_t \*config,\n"
            r"\s+const modem_ssl_credentials_t \*credentials\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t ml307r_ssl_get_context_status",
            self.ml_c,
            re.S | re.M,
        )
        self.assertIsNotNone(provision_fn, "missing ml307r_ssl_provision body")
        body = provision_fn.group("body")

        invalidation = "ml307r_ssl_mark_context(self, config->context_id, MODEM_SSL_AUTH_NONE, false);"
        invalidate_pos = body.find(invalidation)
        self.assertNotEqual(invalidate_pos, -1, "missing pre-provision SSL invalidation")
        before_invalidation = body[:invalidate_pos]

        for token in [
            "MODEM_MQTT_TRANSPORT_TLS",
            "self->mqtt_transport",
            "self->mqtt_ssl_context_id",
            "config->context_id",
            "self->mqtt_session_connected",
            "self->mqtt_data_enabled",
            "ESP_ERR_INVALID_STATE",
        ]:
            self.assertIn(token, before_invalidation)
        self.assertRegex(before_invalidation, r"return\s+ESP_ERR_INVALID_STATE\s*;")

    def test_ml307r_mqtt_configure_sets_cached_before_ssl_binding(self):
        configure_fn = re.search(
            r"^static esp_err_t ml307r_mqtt_configure\(modem_handle_t \*me,\n"
            r"\s+const modem_mqtt_config_t \*config\)\n"
            r"\{(?P<body>.*?)\n\}\n\nstatic esp_err_t ml307r_mqtt_tcp_connect",
            self.ml_c,
            re.S | re.M,
        )
        self.assertIsNotNone(configure_fn, "missing ml307r_mqtt_configure body")
        body = configure_fn.group("body")

        cached_pos = body.find('AT+MQTTCFG=\\"cached\\",0,0')
        ssl_enable_pos = body.find('AT+MQTTCFG=\\"ssl\\",0,1')
        ssl_disable_pos = body.find('AT+MQTTCFG=\\"ssl\\",0,0')
        commit_pos = body.find("free_mqtt_config(&self->mqtt_config);")
        for label, pos in [
            ("cached config", cached_pos),
            ("TLS SSL binding", ssl_enable_pos),
            ("plain SSL binding", ssl_disable_pos),
            ("local config commit", commit_pos),
        ]:
            self.assertNotEqual(pos, -1, f"missing {label} section")

        self.assertLess(cached_pos, ssl_enable_pos)
        self.assertLess(cached_pos, ssl_disable_pos)
        self.assertLess(ssl_enable_pos, commit_pos)
        self.assertLess(ssl_disable_pos, commit_pos)

    def test_examples_support_tls_8883_validation_path(self):
        self.assert_has_all(self.example_kconfig, [
            "EXAMPLE_MQTT_TLS_ENABLE",
            "EXAMPLE_MQTT_TLS_CA_CERT_PEM",
            "default 8883 if EXAMPLE_MQTT_TLS_ENABLE",
        ], "example Kconfig")
        for label, text in [("Air780EP example", self.air_example), ("ML307R example", self.ml_example)]:
            self.assert_has_all(text, [
                "CONFIG_EXAMPLE_MQTT_TLS_ENABLE",
                "lwlte_ssl_provision",
                "lwlte_ssl_get_context_status",
                "LWLTE_MQTT_TRANSPORT_TLS",
                "8883",
                "v1/devices/me/telemetry",
                "v1/devices/me/attributes",
            ], label)

    def test_example_kconfig_tls_help_requires_nonempty_ca(self):
        self.assertIn(
            "Non-empty CA PEM is required when TLS is enabled.",
            self.example_kconfig,
        )

    def test_air_example_rejects_empty_ca_and_requires_server_auth_ca_status(self):
        block = self._example_tls_block(self.air_example, "Air780EP example")
        guard_pos = block.find("ca_pem[0] == '\\0'")
        credentials_pos = block.find("const lwlte_ssl_credentials_t credentials")
        provision_pos = block.find("lwlte_ssl_provision")
        for label, pos in [
            ("empty CA guard", guard_pos),
            ("credentials", credentials_pos),
            ("SSL provision", provision_pos),
        ]:
            self.assertNotEqual(pos, -1, f"Air780EP example missing {label}")
        self.assertLess(guard_pos, credentials_pos)
        self.assertLess(guard_pos, provision_pos)
        self.assertIn(
            "MQTT TLS enabled but CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM is empty",
            block,
        )
        self.assert_has_all(block, [
            "ret == ESP_OK",
            "status.provisioned",
            "status.auth_mode == LWLTE_SSL_AUTH_SERVER",
            "status.ca_cert_present",
        ], "Air780EP TLS status validation")

    def test_ml_example_rejects_empty_ca_and_requires_checked_server_auth_ca_status(self):
        block = self._example_tls_block(self.ml_example, "ML307R example")
        guard_pos = block.find("ca_pem[0] == '\\0'")
        credentials_pos = block.find("const lwlte_ssl_credentials_t credentials")
        provision_pos = block.find("lwlte_ssl_provision")
        for label, pos in [
            ("empty CA guard", guard_pos),
            ("credentials", credentials_pos),
            ("SSL provision", provision_pos),
        ]:
            self.assertNotEqual(pos, -1, f"ML307R example missing {label}")
        self.assertLess(guard_pos, credentials_pos)
        self.assertLess(guard_pos, provision_pos)
        self.assertIn(
            "MQTT TLS enabled but CONFIG_EXAMPLE_MQTT_TLS_CA_CERT_PEM is empty",
            block,
        )
        self.assert_has_all(block, [
            "ret == ESP_OK",
            "status.provisioned",
            "status.auth_mode == LWLTE_SSL_AUTH_SERVER",
            "status.ca_cert_present",
            "status.check_valid",
        ], "ML307R TLS status validation")

    def test_examples_normalize_kconfig_escaped_pem_newlines_before_provision(self):
        for label, text in [("Air780EP example", self.air_example), ("ML307R example", self.ml_example)]:
            block = self._example_tls_block(text, label)
            normalize_pos = block.find("example_normalize_pem_newlines(ca_pem)")
            credentials_pos = block.find("const lwlte_ssl_credentials_t credentials")
            provision_pos = block.find("lwlte_ssl_provision")
            free_pos = block.find("free(ca_pem_normalized)")
            for item_label, pos in [
                ("PEM newline normalization", normalize_pos),
                ("credentials", credentials_pos),
                ("SSL provision", provision_pos),
                ("normalized PEM free", free_pos),
            ]:
                self.assertNotEqual(pos, -1, f"{label} missing {item_label}")

            self.assertLess(normalize_pos, credentials_pos)
            self.assertLess(credentials_pos, provision_pos)
            self.assertLess(provision_pos, free_pos)
            self.assertIn("MQTT TLS CA PEM normalization failed", block)
            self.assertIn(".ca_cert_pem = (const uint8_t *)ca_pem_normalized", block)
            self.assertIn(".ca_cert_len = strlen(ca_pem_normalized)", block)
            self.assertNotIn(".ca_cert_len = strlen(ca_pem)", block)

            self.assertIn("example_normalize_pem_newlines", text)
            self.assertIn("src[1] == 'n'", text)
            self.assertIn("*dst++ = '\\n'", text)
            self.assertIn("src[1] == 'r'", text)
            self.assertIn("*dst++ = '\\r'", text)

    def test_example_pem_newline_normalizer_handles_kconfig_escapes(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("cc compiler unavailable for PEM normalizer behavior check")

        harness = textwrap.dedent(r'''
            static int check_case(const char *input, const char *expected)
            {
                char *actual = example_normalize_pem_newlines(input);
                if (!actual) {
                    return 1;
                }
                int mismatch = strcmp(actual, expected) != 0;
                free(actual);
                return mismatch;
            }

            int main(void)
            {
                if (check_case("BEGIN\\nLINE\\r\\nEND", "BEGIN\nLINE\r\nEND")) {
                    return 2;
                }
                if (check_case("BEGIN\nLINE\nEND", "BEGIN\nLINE\nEND")) {
                    return 3;
                }
                if (check_case("A\\tB\\\\C", "A\\tB\\\\C")) {
                    return 4;
                }
                return 0;
            }
        ''')

        for label, text in [("Air780EP example", self.air_example), ("ML307R example", self.ml_example)]:
            helper = self._example_pem_normalizer(text, label)
            program = "\n".join([
                "#include <stdlib.h>",
                "#include <string.h>",
                helper,
                harness,
            ])
            with tempfile.TemporaryDirectory() as temp_dir:
                source = Path(temp_dir) / "pem_normalizer_test.c"
                binary = Path(temp_dir) / "pem_normalizer_test"
                source.write_text(program, encoding="utf-8")
                compile_result = subprocess.run(
                    [compiler, str(source), "-o", str(binary)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    compile_result.returncode,
                    0,
                    f"{label} PEM normalizer compile failed:\n"
                    f"stdout={compile_result.stdout}\nstderr={compile_result.stderr}",
                )
                run_result = subprocess.run(
                    [str(binary)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    run_result.returncode,
                    0,
                    f"{label} PEM normalizer behavior failed:\n"
                    f"stdout={run_result.stdout}\nstderr={run_result.stderr}",
                )

    def test_roadmap_mentions_selected_ssl_design(self):
        self.assertIn("CORE_CMD_SSL_PROVISION", self.roadmap)
        self.assertIn("CORE_CMD_SSL_GET_CONTEXT_STATUS", self.roadmap)
        self.assertIn("ssl_provision", self.roadmap)
        self.assertIn("ssl_get_context_status", self.roadmap)
        self.assertIn("modem_ssl_context_config_t", self.roadmap)
        self.assertIn("modem_ssl_credentials_t", self.roadmap)
        self.assertNotIn("modem_ssl_config_t", self.roadmap)
        self.assertIn("lwlte_ssl_get_context_status", self.roadmap)
        for stale_op in ["ssl_configure", "ssl_write_cert", "ssl_write_key"]:
            self.assertNotIn(stale_op, self.roadmap)
        self.assertIn("MQTT TLS 验证目标", self.roadmap)


if __name__ == "__main__":
    unittest.main()
