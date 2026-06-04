# ML307R AT Reference Design

## Goal

Add a full ML307R AT/URC reference document that mirrors `docs/agents/at_cmd_air780ep.md` in coverage and table style. The document will support the planned `modem_ml307r_t` subclass by making each modem capability comparable across Air780EP and ML307R.

## Source Materials

Use the ML307R vendor references under `reference/中移物联ML307R/`, especially:

- `AT_Commands_Reference_Guide_4G_Series_V2.0.5.pdf` for base AT commands, SIM, signal, registration, PDP context, errors, and basic module control.
- `ML307R_通信流程示例-V1.1.2.pdf` for startup flow, `+MATREADY`, autobaud behavior, registration flow, and `AT+MIPCALL` application-layer dialing examples.
- `TCP_IP用户手册_5.1.2-R.pdf` for socket, TCP/UDP, DNS, ping, TCP keepalive, and `+MIPURC` data/connection URCs.
- `HTTP_HTTPS用户手_V6.1.1.pdf` and `SSL用户手册_V5.4.2-R.pdf` for HTTP/HTTPS and TLS configuration.
- `MQTT用户手册_V6.8.3.pdf` for MQTT configuration, connect, publish, subscribe, disconnect, state, cache mode, and `+MQTTURC` events.
- `扩展AT用户手册_4G系列-1.7.0.pdf` and related manuals only where they fill the Air780EP document's low-power or wakeup coverage.

## Target File

Create `docs/agents/at_cmd_ml307r.md`.

The file should be a curated implementation reference, not a complete vendor manual copy. It should keep the same intent as the Air780EP document: document the commands and URCs likely needed by the Modem Adapter, plus adjacent TCP/IP, HTTP, MQTT, sleep, and recovery flows.

## Document Structure

Mirror the Air780EP document sections:

- Title and source/manual summary.
- Usage boundary: included and excluded feature areas.
- Implementation notes for AT Engine behavior and URC/query-response overlap.
- Field-description table.
- Module identity and basic control.
- UART/result-code configuration.
- SIM and network status.
- PDP and application-layer dialing.
- PDP event URCs.
- TCP/IP connection layer and ping.
- TCP/IP error/URC notes.
- HTTP/HTTPS commands and URCs.
- MQTT commands and URCs.
- Sleep and low-power commands.
- System URC registration list.
- Later connection-layer URC list.
- Recommended initialization, PDP, socket, HTTP, MQTT, and recovery flows.

## Capability Mapping

Document ML307R commands against the same modem capabilities used by Air780EP. Key differences to make explicit:

- Startup readiness uses `+MATREADY`, not Air780EP `RDY`.
- In autobaud mode there may be no `+MATREADY`; the UART must send `AT` first and wait for `OK` before later commands.
- After `+MATREADY`, the communication-flow guide requires waiting at least 2 seconds before `AT+CFUN=0` or `AT+CFUN=1`.
- Identity and base cellular commands mostly use standard 3GPP commands such as `AT+CGMM`, `AT+CGMR`, `AT+CGSN`, `AT+CPIN?`, `AT+CSQ`, `AT+CESQ`, `AT+CREG?`, `AT+CGREG?`, `AT+CEREG?`, `AT+CGATT?`, `AT+CGDCONT`, `AT+CGACT`, and `AT+CGPADDR`.
- The recommended ML307R PDP/application network path uses `AT+MIPCALL`, not Air780EP `AT+CSTT` / `AT+CIICR` / `AT+CIFSR`.
- Socket operations use `AT+MIPCFG`, `AT+MIPOPEN`, `AT+MIPSEND`, `AT+MIPRD`, `AT+MIPSTATE`, `AT+MIPCLOSE`, `AT+MIPSACK`, `AT+MDNSCFG`, `AT+MDNSGIP`, and `AT+MPING` families instead of Air780EP `AT+CIP*` families.
- MQTT operations use `AT+MQTTCFG`, `AT+MQTTCONN`, `AT+MQTTSUB`, `AT+MQTTUNSUB`, `AT+MQTTPUB`, `AT+MQTTREAD`, `AT+MQTTSTATE`, and `AT+MQTTDISC`, with `+MQTTURC` reporting, instead of Air780EP `AT+MCONFIG`, `AT+MIPSTART`, `AT+MCONNECT`, `AT+MPUB`, `AT+MSUB`, and related ACK lines.
- HTTP/HTTPS and SSL should be mapped from ML307R manuals rather than assuming Air780EP `SAPBR` / `HTTPACTION` / `SSLCFG` details are identical.

## URC Handling Design

The document should separate system-level URCs from connection-layer URCs.

System-level URCs recommended for `modem_ml307r_t` include:

- `+MATREADY` for startup readiness.
- `+CPIN:` for SIM/PIN state changes when the AT Engine dispatches it outside command responses.
- `+CREG:`, `+CGREG:`, and `+CEREG:` for registration state changes.
- PDP/application dialing reports such as `+MIPCALL:` where they indicate data-plane activation or deactivation.

Connection-layer URCs should be documented for later socket/MQTT/HTTP handlers, including:

- `+MIPOPEN:` connection result.
- `+MIPCLOSE:` close result.
- `+MIPSEND:` send accepted result.
- `+MIPURC:` receive, close, error, ACK, and cached-data indications.
- `CONNECT` for transparent-mode connection success.
- `+MQTTURC:` MQTT connect, disconnect, publish, subscribe, unsubscribe, received-message, cached-message, and error events.
- HTTP/HTTPS completion and data-ready URCs from the ML307R HTTP/HTTPS manual.

The implementation note must preserve the existing AT Engine constraint: if a line can be both a query response and a URC, command responses parse it inside the command result, while idle-time lines are dispatched as URCs.

## Recommended Flow Design

The final document should include ML307R-specific flows rather than copying Air780EP flows verbatim:

- Basic startup: wait for `+MATREADY` when fixed baud is used; if no startup URC is received in autobaud mode, send `AT` until `OK`, then send `ATE0` and `AT+CMEE=1`.
- Registration: query `AT+CPIN?`, `AT+CFUN?`, `AT+CEREG?`, and optionally `AT+CGREG?` / `AT+CREG?`; enable URCs where useful.
- PDP/application dialing: use `AT+MIPCALL?`, configure APN with `AT+CGDCONT` when needed, then `AT+MIPCALL=1,<cid>` and wait for `+MIPCALL:<cid>,1,...`; disconnect with `AT+MIPCALL=0,<cid>` and note the vendor warning that this disconnects application networking but does not necessarily deactivate PDP.
- Socket: configure the connection with `AT+MIPCFG`, open with `AT+MIPOPEN`, send with `AT+MIPSEND`, receive either direct `+MIPURC` data or cached data via `AT+MIPRD`, and close with `AT+MIPCLOSE`.
- MQTT: configure with `AT+MQTTCFG`, connect with `AT+MQTTCONN`, wait for `+MQTTURC:"conn"`, publish/subscribe with `AT+MQTTPUB` / `AT+MQTTSUB`, read cached messages with `AT+MQTTREAD` if cache mode is enabled, and disconnect with `AT+MQTTDISC`.
- Error recovery: on data-plane loss, close or release affected socket/MQTT resources, query `AT+MIPCALL?`, and re-run registration plus `MIPCALL` setup as needed.

## Review Criteria

The document is complete when:

- Its top-level coverage is parallel to `docs/agents/at_cmd_air780ep.md`.
- It cites ML307R source PDFs in the introduction.
- The new ML307R reference is indexed in both loaded `AGENTS.md` instruction files.
- Each section maps commands to Modem Adapter semantics, not just vendor chapter names.
- ML307R-specific command families are used instead of Air780EP command names where they differ.
- URCs are split between system-level modem events and later connection-layer handlers.
- The recommended flows are implementable by a future `modem_ml307r_t` without requiring the reader to infer Air780EP differences.

## Out Of Scope

Do not implement `modem_ml307r_t` in this task.
