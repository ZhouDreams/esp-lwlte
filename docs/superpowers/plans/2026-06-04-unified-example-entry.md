# Unified Example Entry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace standalone `examples/*` projects and the root `main/` placeholder with a single root example entry selected by a macro in `example/main.c`.

**Architecture:** The root ESP-IDF project uses `example/` as its only application component. `example/main.c` owns `app_main()` and dispatches to focused example implementation files through declarations in `example/example.h`. MQTT menuconfig symbols move into `example/Kconfig.projbuild` so the root build keeps the existing MQTT settings.

**Tech Stack:** ESP-IDF CMake project, C, FreeRTOS, ESP-IDF UART/GPIO drivers, project `src` component.

---

## File Structure

- Create `example/CMakeLists.txt`: registers the unified example component and all migrated example sources.
- Create `example/Kconfig.projbuild`: preserves `CONFIG_EXAMPLE_MQTT_*` options from the old MQTT example project.
- Create `example/example.h`: declares example selection constants and `example_*_run()` functions.
- Create `example/main.c`: owns `app_main()` and dispatches based on `EXAMPLE_SELECTED`.
- Create `example/basic_connect.c`: migrated from `examples/basic_connect/main/main.c`, with `app_main()` renamed to `example_basic_connect_run()`.
- Create `example/mqtt_client.c`: migrated from `examples/mqtt_client/main/main.c`, with `app_main()` renamed to `example_mqtt_client_run()`.
- Create `example/ml307r_probe.c`: migrated from `examples/basic_connect_ml307r/main/main.c`, with `app_main()` renamed to `example_ml307r_probe_run()`.
- Modify `CMakeLists.txt`: point ESP-IDF at `src` and `example` through `EXTRA_COMPONENT_DIRS`.
- Modify `docs/agents/directory-structure.md`: document `example/` as the single entry and remove standalone example wording.
- Delete `main/CMakeLists.txt` and `main/main.c`.
- Delete old standalone `examples/basic_connect/`, `examples/mqtt_client/`, and `examples/basic_connect_ml307r/` after migration.

## Task 1: Create Unified Example Skeleton

**Files:**
- Create: `example/CMakeLists.txt`
- Create: `example/example.h`
- Create: `example/main.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `example/CMakeLists.txt`**

Add this file:

```cmake
idf_component_register(
    SRCS "main.c"
         "basic_connect.c"
         "mqtt_client.c"
         "ml307r_probe.c"
    INCLUDE_DIRS "."
    REQUIRES src esp_driver_gpio esp_driver_uart
)
```

- [ ] **Step 2: Create `example/example.h`**

Add this file:

```c
#ifndef EXAMPLE_H
#define EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EXAMPLE_BASIC_CONNECT  1
#define EXAMPLE_MQTT_CLIENT    2
#define EXAMPLE_ML307R_PROBE   3

void example_basic_connect_run(void);
void example_mqtt_client_run(void);
void example_ml307r_probe_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXAMPLE_H */
```

- [ ] **Step 3: Create `example/main.c`**

Add this file:

```c
/**
 * @file main.c
 * @brief Unified example entry
 * @details Select one example by editing EXAMPLE_SELECTED.
 */

/*********************
 *      INCLUDES
 *********************/
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "example.h"

/*********************
 *      DEFINES
 *********************/
#define TAG                 "example"

#define EXAMPLE_SELECTED    EXAMPLE_BASIC_CONNECT

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void idle_forever(void);

/**********************
 *   GLOBAL FUNCTIONS
 **********************/
void app_main(void)
{
    switch (EXAMPLE_SELECTED) {
    case EXAMPLE_BASIC_CONNECT:
        example_basic_connect_run();
        break;
    case EXAMPLE_MQTT_CLIENT:
        example_mqtt_client_run();
        break;
    case EXAMPLE_ML307R_PROBE:
        example_ml307r_probe_run();
        break;
    default:
        ESP_LOGE(TAG, "invalid EXAMPLE_SELECTED=%d", EXAMPLE_SELECTED);
        idle_forever();
        break;
    }
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void idle_forever(void)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 4: Modify root `CMakeLists.txt`**

Replace the file with:

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/src
                         ${CMAKE_CURRENT_LIST_DIR}/example)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp-lwlte)
```

- [ ] **Step 5: Run configure check expecting missing migrated sources**

Run: `idf.py reconfigure`

Expected: configuration fails because `example/basic_connect.c`, `example/mqtt_client.c`, and `example/ml307r_probe.c` do not exist yet. This confirms CMake is using the new `example/` component.

## Task 2: Migrate Existing Example Sources

**Files:**
- Create: `example/basic_connect.c`
- Create: `example/mqtt_client.c`
- Create: `example/ml307r_probe.c`

- [ ] **Step 1: Create `example/basic_connect.c` from the old source**

Copy the contents of `examples/basic_connect/main/main.c` to `example/basic_connect.c`, then make these exact edits:

```diff
+#include "example.h"
...
-void app_main(void)
+void example_basic_connect_run(void)
```

Keep all existing helper functions, constants, and runtime behavior unchanged.

- [ ] **Step 2: Create `example/mqtt_client.c` from the old source**

Copy the contents of `examples/mqtt_client/main/main.c` to `example/mqtt_client.c`, then make these exact edits:

```diff
+#include "example.h"
...
-void app_main(void)
+void example_mqtt_client_run(void)
```

Keep all existing MQTT logic unchanged, including `CONFIG_EXAMPLE_MQTT_*` usage.

- [ ] **Step 3: Create `example/ml307r_probe.c` from the old source**

Copy the contents of `examples/basic_connect_ml307r/main/main.c` to `example/ml307r_probe.c`, then make these exact edits:

```diff
+#include "example.h"
...
-void app_main(void)
+void example_ml307r_probe_run(void)
```

Keep all UART probe logic unchanged.

- [ ] **Step 4: Run default build**

Run: `idf.py build`

Expected: build reaches compilation of the unified root project. If it fails on missing `CONFIG_EXAMPLE_MQTT_*`, continue to Task 3 before treating it as a code error.

## Task 3: Migrate MQTT Kconfig And Documentation

**Files:**
- Create: `example/Kconfig.projbuild`
- Modify: `docs/agents/directory-structure.md`

- [ ] **Step 1: Create `example/Kconfig.projbuild`**

Add this file:

```kconfig
menu "Example MQTT Settings"

config EXAMPLE_MQTT_HOST
    string "MQTT broker host"
    default "admin.jovisdreams.site"

config EXAMPLE_MQTT_PORT
    int "MQTT broker port"
    range 1 65535
    default 1883

config EXAMPLE_MQTT_CLIENT_ID
    string "MQTT client ID"
    default "esp-lwlte-mqtt-example"

config EXAMPLE_MQTT_TOKEN
    string "ThingsBoard device access token (used as MQTT username)"
    default "Air780EP"

config EXAMPLE_MQTT_KEEPALIVE_S
    int "MQTT keepalive seconds"
    range 10 1200
    default 120

endmenu
```

- [ ] **Step 2: Update `docs/agents/directory-structure.md` top-level tree**

Change the tree to:

```text
esp-lwlte/
├── src/           # 组件源码
├── example/       # 统一示例入口
├── docs/          # 项目文档
└── reference/     # 只读参考文档（git ignore）
```

- [ ] **Step 3: Update `docs/agents/directory-structure.md` example section**

Replace the `examples/` section with:

```markdown
### example/ — 统一示例入口

根 ESP-IDF 项目的 main 组件。`example/main.c` 是唯一示例入口，通过 `EXAMPLE_SELECTED` 宏选择要构建烧录后运行的示例。

示例实现按文件拆分，例如 `basic_connect.c`、`mqtt_client.c`、`ml307r_probe.c`。新增示例时应在 `example/example.h` 中新增选择宏和 run 函数声明，并在 `example/main.c` 的选择逻辑中接入。
```

- [ ] **Step 4: Run reconfigure**

Run: `idf.py reconfigure`

Expected: configuration succeeds and generated config contains `CONFIG_EXAMPLE_MQTT_HOST`, `CONFIG_EXAMPLE_MQTT_PORT`, `CONFIG_EXAMPLE_MQTT_CLIENT_ID`, `CONFIG_EXAMPLE_MQTT_TOKEN`, and `CONFIG_EXAMPLE_MQTT_KEEPALIVE_S`.

## Task 4: Remove Old Standalone Entry Points

**Files:**
- Delete: `main/CMakeLists.txt`
- Delete: `main/main.c`
- Delete: `examples/basic_connect/`
- Delete: `examples/mqtt_client/`
- Delete: `examples/basic_connect_ml307r/`

- [ ] **Step 1: Delete the old root placeholder main component**

Remove:

```text
main/CMakeLists.txt
main/main.c
```

- [ ] **Step 2: Delete old standalone example projects**

Remove these directories after confirming their sources and MQTT Kconfig were migrated:

```text
examples/basic_connect/
examples/mqtt_client/
examples/basic_connect_ml307r/
```

- [ ] **Step 3: Search for old path references**

Run: `rg "examples/(basic_connect|mqtt_client|basic_connect_ml307r)|main/main\.c|examples/" AGENTS.md docs CMakeLists.txt example src`

Expected: no stale references that describe current build behavior. References in historical `docs/superpowers/specs/` or `docs/superpowers/plans/` may remain because they document past work.

## Task 5: Verify All Selectable Examples Build

**Files:**
- Temporarily modify: `example/main.c`

- [ ] **Step 1: Build default selection**

Ensure `example/main.c` contains:

```c
#define EXAMPLE_SELECTED    EXAMPLE_BASIC_CONNECT
```

Run: `idf.py build`

Expected: build succeeds for `EXAMPLE_BASIC_CONNECT`.

- [ ] **Step 2: Build MQTT selection**

Temporarily change the selection to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_MQTT_CLIENT
```

Run: `idf.py build`

Expected: build succeeds for `EXAMPLE_MQTT_CLIENT` and uses MQTT config symbols from `example/Kconfig.projbuild`.

- [ ] **Step 3: Build ML307R probe selection**

Temporarily change the selection to:

```c
#define EXAMPLE_SELECTED    EXAMPLE_ML307R_PROBE
```

Run: `idf.py build`

Expected: build succeeds for `EXAMPLE_ML307R_PROBE`.

- [ ] **Step 4: Restore default selection**

Restore:

```c
#define EXAMPLE_SELECTED    EXAMPLE_BASIC_CONNECT
```

- [ ] **Step 5: Inspect working tree**

Run: `git status --short`

Expected: only planned files are added, modified, or deleted. Do not commit unless the user explicitly asks for a commit.

## Self-Review

- Spec coverage: Tasks cover unified `example/main.c`, per-example source files, `example/example.h`, CMake routing, MQTT Kconfig migration, deletion of old `main/` and standalone examples, documentation update, and build verification.
- Placeholder scan: The plan contains no `TBD`, deferred implementation, or unspecified error handling.
- Type consistency: Example IDs are defined once in `example/example.h`; run function names match the dispatcher and migrated implementations.
