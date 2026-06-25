# ML307R ESP32-C3 Pin Assignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ML307R examples use dedicated ESP32-C3 `GPIO3`/`GPIO10`/`GPIO4` defaults so ML307R and Air780EP can stay wired to one development board without IO conflicts.

**Architecture:** Preserve the current one-example-at-a-time `EXAMPLE_SELECTED` runtime model. Change only ML307R example board wiring constants and the example-facing wiring documentation; Air780EP examples and LWLTE internals remain unchanged. Add static host coverage so the ML307R examples cannot silently regress to the Air780EP `GPIO0`/`GPIO1`/`GPIO2` wiring.

**Tech Stack:** C, ESP-IDF, Python `unittest`, Markdown documentation.

---

## File Structure

- Modify `tests/host/test_ml307r_examples_contract.py`: add ML307R TCP example coverage, a regex helper for GPIO macro assertions, and a static regression test for the dedicated ESP32-C3 pin assignment and README wiring table.
- Modify `example/ml307r_basic_connect.c`: change ML307R basic-connect hardware defaults to `TX=GPIO3`, `RX=GPIO10`, `EN=GPIO4`.
- Modify `example/ml307r_tcp_client.c`: change ML307R TCP hardware defaults to `TX=GPIO3`, `RX=GPIO10`, `EN=GPIO4`.
- Modify `example/ml307r_mqtt_client.c`: change ML307R MQTT hardware defaults to `TX=GPIO3`, `RX=GPIO10`, `EN=GPIO4`.
- Modify `example/README.md`: update the ML307R wiring table and state that ML307R intentionally avoids the Air780EP `GPIO0`/`GPIO1`/`GPIO2` wiring.

No commit is included in the required tasks. Repository policy requires explicit user authorization before every `git commit`.

### Task 1: Add Static Pin Assignment Contract

**Files:**
- Modify: `tests/host/test_ml307r_examples_contract.py`

- [ ] **Step 1: Write the failing test**

Edit `tests/host/test_ml307r_examples_contract.py` so the top imports include `re`:

```python
#!/usr/bin/env python3
"""Static regression checks for ML307R examples."""

from pathlib import Path
import re
import unittest
```

Add the TCP example path after `MQTT_C`:

```python
BASIC_C = EXAMPLE_DIR / "ml307r_basic_connect.c"
MQTT_C = EXAMPLE_DIR / "ml307r_mqtt_client.c"
TCP_C = EXAMPLE_DIR / "ml307r_tcp_client.c"
PROBE_C = EXAMPLE_DIR / "ml307r_probe.c"
```

In `setUpClass()`, read the TCP example source after `mqtt_c`:

```python
    @classmethod
    def setUpClass(cls):
        cls.basic_c = read_optional(BASIC_C)
        cls.mqtt_c = read_optional(MQTT_C)
        cls.tcp_c = read_optional(TCP_C)
        cls.example_h = read_optional(EXAMPLE_H)
        cls.main_c = read_optional(MAIN_C)
        cls.cmake = read_optional(CMAKE)
        cls.readme = read_optional(README)
        cls.dir_structure = read_optional(DIR_STRUCTURE)
```

Add this helper after `assert_contains_all()`:

```python
    def assert_pin_define(self, text: str, macro: str, gpio: int, label: str):
        self.assertRegex(
            text,
            rf"#define\s+{re.escape(macro)}\s+GPIO_NUM_{gpio}\b",
            label,
        )
```

Add this test after `test_example_selection_wiring()`:

```python
    def test_ml307r_examples_use_dedicated_esp32c3_pins(self):
        for label, text in [
            ("ml307r_basic_connect.c", self.basic_c),
            ("ml307r_mqtt_client.c", self.mqtt_c),
            ("ml307r_tcp_client.c", self.tcp_c),
        ]:
            self.assert_pin_define(text, "EXAMPLE_LTE_UART_TX_PIN", 3, label)
            self.assert_pin_define(text, "EXAMPLE_LTE_UART_RX_PIN", 10, label)
            self.assert_pin_define(text, "EXAMPLE_LTE_EN_PIN", 4, label)
            self.assertNotRegex(
                text,
                r"#define\s+EXAMPLE_LTE_UART_TX_PIN\s+GPIO_NUM_0\b",
                label,
            )
            self.assertNotRegex(
                text,
                r"#define\s+EXAMPLE_LTE_UART_RX_PIN\s+GPIO_NUM_1\b",
                label,
            )
            self.assertNotRegex(
                text,
                r"#define\s+EXAMPLE_LTE_EN_PIN\s+GPIO_NUM_2\b",
                label,
            )

        self.assert_contains_all(self.readme, [
            "| GPIO3 | RX | ESP32-C3 UART1 TX |",
            "| GPIO10 | TX | ESP32-C3 UART1 RX |",
            "| GPIO4 | EN or power enable | Controlled by modem adapter |",
            "ML307R defaults intentionally avoid the Air780EP GPIO0/GPIO1/GPIO2 wiring",
        ])
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_examples_contract.py -v
```

Expected: `test_ml307r_examples_use_dedicated_esp32c3_pins` fails because the ML307R examples and README still contain the old `GPIO0`/`GPIO1`/`GPIO2` wiring.

### Task 2: Change ML307R Example GPIO Defaults

**Files:**
- Modify: `example/ml307r_basic_connect.c`
- Modify: `example/ml307r_tcp_client.c`
- Modify: `example/ml307r_mqtt_client.c`
- Test: `tests/host/test_ml307r_examples_contract.py`

- [ ] **Step 1: Update `example/ml307r_basic_connect.c` pin defines**

Replace the LTE hardware define block with:

```c
#define EXAMPLE_LTE_UART_NUM                 UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN              GPIO_NUM_3
#define EXAMPLE_LTE_UART_RX_PIN              GPIO_NUM_10
#define EXAMPLE_LTE_EN_PIN                   GPIO_NUM_4
#define EXAMPLE_LTE_UART_BAUD_RATE           115200
#define EXAMPLE_LTE_APN                      ""
#define EXAMPLE_LTE_PRIMARY_CID              1
```

- [ ] **Step 2: Update `example/ml307r_tcp_client.c` pin defines**

Replace the LTE hardware define block with:

```c
#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_3
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_10
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_4
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1
```

- [ ] **Step 3: Update `example/ml307r_mqtt_client.c` pin defines**

Replace the LTE hardware define block with:

```c
#define EXAMPLE_LTE_UART_NUM                     UART_NUM_1
#define EXAMPLE_LTE_UART_TX_PIN                  GPIO_NUM_3
#define EXAMPLE_LTE_UART_RX_PIN                  GPIO_NUM_10
#define EXAMPLE_LTE_EN_PIN                       GPIO_NUM_4
#define EXAMPLE_LTE_UART_BAUD_RATE               115200
#define EXAMPLE_LTE_APN                          ""
#define EXAMPLE_LTE_PRIMARY_CID                  1
```

- [ ] **Step 4: Run the static test and verify only README wiring remains failing**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_examples_contract.py -v
```

Expected: the GPIO macro assertions pass for the three C files. The same test still fails on README tokens because `example/README.md` has not been updated yet.

### Task 3: Update ML307R Wiring Documentation

**Files:**
- Modify: `example/README.md`
- Test: `tests/host/test_ml307r_examples_contract.py`

- [ ] **Step 1: Update the ML307R wiring table**

In `example/README.md`, replace the ML307R wiring table rows with:

```markdown
| ESP32-C3 | ML307R | Notes |
|----------|--------|-------|
| GPIO3 | RX | ESP32-C3 UART1 TX |
| GPIO10 | TX | ESP32-C3 UART1 RX |
| GPIO4 | EN or power enable | Controlled by modem adapter |
| GND | GND | Common ground required |
```

Replace the paragraph after the table with:

```markdown
ML307R defaults intentionally avoid the Air780EP GPIO0/GPIO1/GPIO2 wiring so both modules can stay connected while `EXAMPLE_SELECTED` chooses which one runs. The ML307R modem adapter toggles EN low then high during start/reset, probes `AT` until `OK`, and then sends basic AT initialization commands. It does not use the old standalone UART diagnostic path.
```

- [ ] **Step 2: Run the static test and verify it passes**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_examples_contract.py -v
```

Expected: all tests in `test_ml307r_examples_contract.py` pass.

### Task 4: Full Verification

**Files:**
- Verify only: `tests/host/test_ml307r_examples_contract.py`
- Verify only: `example/ml307r_basic_connect.c`
- Verify only: `example/ml307r_tcp_client.c`
- Verify only: `example/ml307r_mqtt_client.c`
- Verify only: `example/README.md`

- [ ] **Step 1: Run the targeted host test**

Run:

```bash
python3 -m unittest tests/host/test_ml307r_examples_contract.py -v
```

Expected: `OK`.

- [ ] **Step 2: Run whitespace validation**

Run:

```bash
git diff --check
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Build for ESP32-C3 with MCP tools**

Use the ESP-IDF MCP build tools in this order:

```text
esp-idf-eim_set_target target=esp32c3
esp-idf-eim_build_project
```

Expected: target selection succeeds and the project build completes without errors.

If the MCP tools are unavailable during execution, run this fallback shell command from the repository root:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py set-target esp32c3 && idf.py build
```

Expected: `idf.py build` completes without errors.

### Task 5: Optional User-Authorized Commit

**Files:**
- Stage only: `docs/superpowers/specs/2026-06-24-ml307r-c3-pin-assignment-design.md`
- Stage only: `docs/superpowers/plans/2026-06-24-ml307r-c3-pin-assignment.md`
- Stage only: `tests/host/test_ml307r_examples_contract.py`
- Stage only: `example/ml307r_basic_connect.c`
- Stage only: `example/ml307r_tcp_client.c`
- Stage only: `example/ml307r_mqtt_client.c`
- Stage only: `example/README.md`

- [ ] **Step 1: Ask for explicit commit authorization**

Ask the user whether to commit these changes. Do not commit unless the user explicitly says to commit.

- [ ] **Step 2: Inspect status and diff before committing**

Run:

```bash
git status --short
git diff -- docs/superpowers/specs/2026-06-24-ml307r-c3-pin-assignment-design.md docs/superpowers/plans/2026-06-24-ml307r-c3-pin-assignment.md tests/host/test_ml307r_examples_contract.py example/ml307r_basic_connect.c example/ml307r_tcp_client.c example/ml307r_mqtt_client.c example/README.md
git log --oneline -10
```

Expected: the diff contains only the spec, plan, ML307R pin assignment test, three ML307R example pin updates, and README wiring update. Existing unrelated local edits in `example/Kconfig.projbuild` and `tests/host/test_tcp_client_end_to_end_contract.py` remain unstaged.

- [ ] **Step 3: Commit only after authorization**

Run only after explicit user approval:

```bash
git add docs/superpowers/specs/2026-06-24-ml307r-c3-pin-assignment-design.md docs/superpowers/plans/2026-06-24-ml307r-c3-pin-assignment.md tests/host/test_ml307r_examples_contract.py example/ml307r_basic_connect.c example/ml307r_tcp_client.c example/ml307r_mqtt_client.c example/README.md
git commit -m "fix(example): assign dedicated ML307R ESP32-C3 pins"
```

Expected: commit succeeds and does not include unrelated local changes.
