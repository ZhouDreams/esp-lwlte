# ML307R CEREG-Only Registration Design

## Context

Hardware testing on the ESP32-C3 + ML307R platform showed that the ML307R UART path is working on the dedicated pins, but all three ML307R examples fail during modem startup. The common failure is `AT+CGREG=2` returning `ERROR` three times.

Temporary diagnostic firmware confirmed the current ML307R module firmware behavior:

- `AT+CGREG?`, `AT+CGREG=0`, `AT+CGREG=1`, and `AT+CGREG=2` return `ERROR`.
- `AT+CREG?` and `AT+CREG=2` return `ERROR`.
- `AT+CEREG?` returns a valid `+CEREG:` response and `OK`.
- `AT+CEREG=1` returns `OK`.
- The module later reports `+CEREG: 1`, confirming LTE/EPS registration progress.

The current ML307R implementation treats `CGREG` and `CREG` as required startup and fallback registration commands. That assumption matches older docs and tests, but it does not match the tested ML307R firmware.

## Goal

Make ML307R registration handling use `CEREG` as the only active registration path so unsupported `CGREG` and `CREG` commands cannot fail startup or later registration polling.

## Chosen Approach

Apply a focused ML307R-only change:

- In `src/modem/modem_ml307r.c`, remove `AT+CGREG=2` and `AT+CREG=2` from `run_basic_init_cmds()`.
- In `src/modem/modem_ml307r.c`, make `ml307r_get_registration()` query only `AT+CEREG?`.
- In `src/modem/modem_ml307r.c`, stop registering and unregistering `+CREG:` and `+CGREG:` URC handlers for ML307R; keep `+CEREG:`.
- Remove or leave unused ML307R CREG/CGREG definitions only if doing so keeps the file cleaner without broad refactoring.
- Leave Air780EP behavior unchanged.

This is intentionally not a generic “ignore any unsupported init command” mechanism. The observed incompatibility is specific and repeatable, and startup should remain strict for commands ML307R actually needs.

## Documentation Updates

Update ML307R-specific documentation to state that the tested ML307R firmware uses `CEREG` only for registration status and URC enablement:

- `docs/agents/at_cmd_ml307r.md`
- `docs/modem-init-min-flow.md`

Do not remove Air780EP `CGREG/CREG` documentation. Air780EP retains its current broader registration command set.

## Tests

Update `tests/host/test_ml307r_contract.py` so the static contract matches the new ML307R behavior:

- Startup init order is `ATE0`, `AT+CMEE=1`, `AT+CEREG=2` only.
- ML307R identity/status/registration mapping expects `AT+CEREG?` but not `AT+CGREG?` or `AT+CREG?`.
- ML307R URC registration expects `ML307R_URC_CEREG` but not `ML307R_URC_CREG` or `ML307R_URC_CGREG`.

Existing Air780EP tests must continue to expect the Air780EP `CEREG/CGREG/CREG` behavior.

## Verification

Verification should include:

- Run the updated ML307R host contract test.
- Run the Air780EP command-gated init host test to confirm Air780EP behavior was not changed.
- Run `git diff --check`.
- Build the ESP-IDF project for `esp32c3`.
- Flash and monitor at least `EXAMPLE_ML307R_BASIC_CONNECT` to confirm startup passes beyond the previous `AT+CGREG=2` failure point.

If the module later fails at SIM, attach, APN, PDP, ping, MQTT, or TCP stages, treat that as a separate follow-up issue because this change only addresses the proven unsupported registration command family.
