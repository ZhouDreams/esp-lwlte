# Basic Connect ML307R UART Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a standalone ML307R example that probes UART readiness with `ATE0` and `AT` without using esp-lwlte library initialization.

**Architecture:** The example copies the ESP-IDF project layout from `examples/basic_connect`, but its `main.c` only owns direct UART/GPIO probing. It does not call `lwlte_air780ep_init()`, wait for `RDY`, or attempt LTE network activation.

**Tech Stack:** ESP-IDF C, `driver/uart.h`, `driver/gpio.h`, FreeRTOS delays, ESP logging.

---

## File Structure

- Create `examples/basic_connect_ml307r/CMakeLists.txt`: ESP-IDF project entry with `EXTRA_COMPONENT_DIRS` pointing at `../../src` for consistency with existing examples.
- Create `examples/basic_connect_ml307r/main/CMakeLists.txt`: registers only `main.c` and requires UART/GPIO drivers.
- Create `examples/basic_connect_ml307r/main/main.c`: configures UART, drives EN high, sends `ATE0` and repeated `AT`, logs raw received bytes.
- Create `examples/basic_connect_ml307r/sdkconfig.defaults`: copies ESP32-C3 defaults from the existing basic example.
- Create `examples/basic_connect_ml307r/README.md`: documents ML307R `+MATREADY` versus autobaud no-URC behavior and build/run steps.

### Task 1: Create ML307R UART Probe Example

**Files:**
- Create: `examples/basic_connect_ml307r/CMakeLists.txt`
- Create: `examples/basic_connect_ml307r/main/CMakeLists.txt`
- Create: `examples/basic_connect_ml307r/main/main.c`
- Create: `examples/basic_connect_ml307r/sdkconfig.defaults`
- Create: `examples/basic_connect_ml307r/README.md`

- [ ] **Step 1: Create project build files**

Use the same project shape as `examples/basic_connect`, with project name `basic_connect_ml307r`.

- [ ] **Step 2: Write direct UART probe implementation**

Implement `app_main()` to initialize UART1 at 115200, set TX GPIO0 and RX GPIO1, set EN GPIO2 high, drain startup bytes, send `ATE0\r\n`, send `AT\r\n` up to three times, and log raw responses.

- [ ] **Step 3: Write README**

Document that ML307R flow guide shows `+MATREADY`, while autobaud mode may emit no startup URC and requires sending `AT` before later commands.

- [ ] **Step 4: Build verification**

Run: `idf.py -C examples/basic_connect_ml307r build`
Expected: build succeeds with no compile errors.

## Self-Review

- Spec coverage: The plan creates only an example copy and does not change library code, matching the requested scope.
- Placeholder scan: No placeholders remain.
- Type consistency: All files use ESP-IDF C APIs already used by the project examples.
