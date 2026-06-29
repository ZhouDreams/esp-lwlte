#!/usr/bin/env python3
"""Static end-to-end contract checks for HTTP Service v1 implementation."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class HttpEndToEndContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.modem_c = read("src/modem/modem.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")

    def assert_has_all(self, text: str, tokens: list, label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_public_http_api_exists(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_HTTP_METHOD_GET",
            "LWLTE_HTTP_METHOD_POST",
            "LWLTE_HTTP_TRANSPORT_HTTP",
            "LWLTE_HTTP_TRANSPORT_HTTPS",
            "lwlte_http_method_t method;",
            "const char *url;",
            "lwlte_http_transport_t transport;",
            "uint8_t ssl_context_id;",
            "int status_code;",
            "uint8_t *body;",
            "size_t body_len;",
            "int modem_error_code;",
            "esp_err_t lwlte_http_request(lwlte_handle_t me,",
            "void lwlte_http_response_release(",
        ], "lwlte.h HTTP API")

    def test_facade_implementation_threads_http(self):
        self.assert_has_all(self.lwlte_c, [
            "facade_http_cmd_done_cb",
            "CORE_CMD_HTTP_REQUEST",
            ".done_cb = facade_http_cmd_done_cb",
            "lwlte_http_response_release",
            "free(response->body)",
        ], "lwlte.c HTTP facade")

    def test_core_command_and_result_types(self):
        self.assert_has_all(self.core_h, [
            "CORE_CMD_HTTP_REQUEST",
            "core_http_result_t",
            "lwlte_http_method_t method;",
            "lwlte_http_transport_t transport;",
        ], "core.h HTTP types")
        self.assert_has_all(self.core_c, [
            "CORE_CMD_HTTP_REQUEST",
            "clone->data.http_request.url",
            "clone->data.http_request.content_type",
            "clone->data.http_request.body",
        ], "core.c HTTP clone")
        self.assert_has_all(self.core_fsm_c, [
            "CORE_CMD_HTTP_REQUEST",
            "modem_http_request(me->modem",
            ".method = (modem_http_method_t)",
            ".transport = (modem_http_transport_t)",
            "core_http_result_t result",
        ], "core_fsm.c HTTP mapping")

    def test_modem_types_and_wrapper(self):
        self.assert_has_all(self.modem_h, [
            "MODEM_HTTP_METHOD_GET",
            "MODEM_HTTP_METHOD_POST",
            "MODEM_HTTP_TRANSPORT_HTTP",
            "MODEM_HTTP_TRANSPORT_HTTPS",
            "modem_http_request_t",
            "modem_http_response_t",
            "esp_err_t modem_http_request(modem_handle_t me,",
        ], "modem.h HTTP types")
        self.assert_has_all(self.modem_c, [
            "modem_http_request(modem_handle_t me,",
            "me->ops->http_request",
        ], "modem.c HTTP wrapper")

    def test_air780ep_http_ops_and_at_commands(self):
        self.assert_has_all(self.air_c, [
            ".http_request = air780ep_http_request",
            "AT+HTTPINIT",
            "AT+HTTPSSL=1",
            "AT+HTTPSSL=0",
            'AT+HTTPPARA="CID"',
            'AT+HTTPPARA="URL"',
            "AT+HTTPDATA=",
            "AT+HTTPACTION=",
            "+HTTPACTION:",
            "AT+HTTPREAD",
            "AT+HTTPTERM",
        ], "air780ep HTTP AT commands")

    def test_ml307r_http_ops_and_at_commands(self):
        self.assert_has_all(self.ml_c, [
            ".http_request = ml307r_http_request",
            "AT+MHTTPCREATE",
            "AT+MHTTPCFG=",
            "AT+MHTTPHEADER=",
            "AT+MHTTPCONTENT=",
            "AT+MHTTPREQUEST=",
            '+MHTTPURC: "rsp"',
            "AT+MHTTPREAD",
            "AT+MHTTPDEL=",
        ], "ml307r HTTP AT commands")


if __name__ == "__main__":
    unittest.main()
