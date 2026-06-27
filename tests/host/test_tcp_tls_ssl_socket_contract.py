#!/usr/bin/env python3
"""Static contract checks for TCP over TLS (SSL socket) reuse of SSL context."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


def read(rel_path: str) -> str:
    path = ROOT / rel_path
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8")


class TcpTlsSslSocketContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lwlte_h = read("src/include/lwlte.h")
        cls.lwlte_c = read("src/lwlte/lwlte.c")
        cls.core_h = read("src/core/core.h")
        cls.core_c = read("src/core/core.c")
        cls.core_fsm_c = read("src/core/core_fsm.c")
        cls.modem_h = read("src/modem/modem.h")
        cls.tcp_h = read("src/tcp_client/tcp_client.h")
        cls.tcp_c = read("src/tcp_client/tcp_client.c")
        cls.air_c = read("src/modem/modem_air780ep.c")
        cls.ml_c = read("src/modem/modem_ml307r.c")
        cls.kconfig = read("example/Kconfig.projbuild")
        cls.air_example = read("example/air780ep_tcp_client.c")
        cls.ml_example = read("example/ml307r_tcp_client.c")

    def assert_has_all(self, text: str, tokens: list[str], label: str):
        for token in tokens:
            self.assertIn(token, text, f"{label} missing {token}")

    def test_public_tcp_transport_api_exists(self):
        self.assert_has_all(self.lwlte_h, [
            "LWLTE_TCP_TRANSPORT_PLAIN_TCP",
            "LWLTE_TCP_TRANSPORT_TLS",
            "lwlte_tcp_transport_t transport;",
            "uint8_t ssl_context_id;",
        ], "lwlte.h TCP transport")

    def test_core_and_tcp_thread_transport(self):
        self.assert_has_all(self.core_h, [
            "CORE_SOCKET_TRANSPORT_PLAIN_TCP",
            "CORE_SOCKET_TRANSPORT_TLS",
            "core_socket_transport_t transport;",
        ], "core.h socket transport")
        self.assert_has_all(self.core_c, [
            "clone->data.socket_open.transport",
            "clone->data.socket_open.ssl_context_id",
        ], "core.c socket clone")
        self.assert_has_all(self.core_fsm_c, [
            ".transport =",
            ".ssl_context_id =",
        ], "core_fsm.c socket mapping")
        self.assert_has_all(self.tcp_h + self.tcp_c, [
            "tcp_client_open_config_t",
            ".transport =",
            ".ssl_context_id =",
        ], "tcp_client transport threading")

    def test_modem_socket_transport_and_module_tls_mappings(self):
        self.assert_has_all(self.modem_h, [
            "MODEM_SOCKET_TRANSPORT_PLAIN_TCP",
            "MODEM_SOCKET_TRANSPORT_TLS",
            "modem_socket_transport_t transport;",
        ], "modem.h socket transport")
        self.assert_has_all(self.air_c, [
            "AIR780EP_SSL_TCP_CONTEXT_ID",
            "AT+CIPSSL=1",
            "AT+CIPSSL=0",
            'AT+SSLCFG="hostname",0,',
        ], "air780ep socket TLS")
        self.assert_has_all(self.ml_c, [
            'AT+MIPCFG="ssl",',
            "ml307r_ssl_context_marked",
        ], "ml307r socket TLS")

    def test_example_tls_config(self):
        self.assert_has_all(self.kconfig, [
            "EXAMPLE_TCP_TLS_ENABLE",
            "EXAMPLE_TCP_TLS_CA_CERT_PEM",
        ], "example Kconfig TCP TLS")
        self.assert_has_all(self.air_example + self.ml_example, [
            "LWLTE_TCP_TRANSPORT_TLS",
            "lwlte_ssl_provision",
        ], "tcp examples TLS path")


if __name__ == "__main__":
    unittest.main()
