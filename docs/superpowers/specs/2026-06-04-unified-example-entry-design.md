# Unified Example Entry Design

## Goal

Use one ESP-IDF project entry for all examples. The root project should build and flash from `example/main.c`; developers select which example runs by editing a macro in that file. The existing root `main/` placeholder is no longer needed.

## Current State

- The repository root is an ESP-IDF project with `CMakeLists.txt` and a minimal `main/main.c` placeholder.
- Each current example under `examples/*` is its own ESP-IDF project with its own `main/main.c`.
- `examples/mqtt_client/main/Kconfig.projbuild` defines `CONFIG_EXAMPLE_MQTT_*` options used by the MQTT example.

## Selected Approach

Create a single `example/` main component for the root project:

- `example/main.c` owns the only `app_main()`.
- `example/main.c` defines `EXAMPLE_SELECTED` and dispatches to one selected example function.
- Each example implementation lives in a separate source file, such as `example/basic_connect.c`, `example/mqtt_client.c`, and `example/ml307r_probe.c`.
- A shared header, `example/example.h`, declares example IDs and run functions.
- The old root `main/` directory is removed.
- The old standalone `examples/*` projects are removed after their source and configuration are migrated.

## Example Selection

`example/main.c` exposes a simple edit point:

```c
#define EXAMPLE_SELECTED EXAMPLE_BASIC_CONNECT
```

Supported values initially map to the existing examples:

- `EXAMPLE_BASIC_CONNECT`
- `EXAMPLE_MQTT_CLIENT`
- `EXAMPLE_ML307R_PROBE`

`app_main()` uses a `switch` to call the selected run function. Unsupported values log an error and idle rather than silently running the wrong example.

## Build And Configuration

- Root `CMakeLists.txt` keeps `src` as an extra component and adds `example` as the main component path in the minimal ESP-IDF-compatible way.
- `example/CMakeLists.txt` registers all example source files and depends on `src`, `esp_driver_gpio`, and `esp_driver_uart`.
- MQTT configuration moves from `examples/mqtt_client/main/Kconfig.projbuild` to `example/Kconfig.projbuild` so `CONFIG_EXAMPLE_MQTT_*` remains available when building the root project.

## Documentation Updates

- Update `docs/agents/directory-structure.md` so `example/` is documented as the single root example entry.
- Remove wording that says each example is an independent ESP-IDF project.
- Existing example READMEs may be removed with their old standalone project folders unless they contain information not represented elsewhere.

## Error Handling

- Existing example runtime error handling remains unchanged as much as possible.
- The selector handles invalid `EXAMPLE_SELECTED` values explicitly by logging the invalid value and entering a delay loop.

## Verification

- Build the root project with the default `EXAMPLE_SELECTED` value.
- If practical, temporarily switch the selector to each migrated example and build each variant.
- Do not perform flashing unless explicitly requested.

## Out Of Scope

- Adding menuconfig-based example selection.
- Creating new examples.
- Refactoring duplicated helper functions across examples beyond what is necessary for compilation.
- Preserving standalone `examples/*` project builds.
