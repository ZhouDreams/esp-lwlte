# Lifecycle API Unification Design

## Context

The project currently uses both `xxx_create()` and `xxx_init()` patterns. The intended semantics are clear but not fully enforced across all internal modules: user-facing APIs should keep the `esp_err_t xxx_init(config, out)` style, independent opaque heap objects should use `create/destroy`, and embedded/base/composed objects should use `init/deinit`.

`destroy` and `deinit` remain `esp_err_t` returning functions in this project. `init` also returns `esp_err_t`. `create` returns the created object pointer and reports failure as `NULL`.

## Goals

- Keep user API shape unchanged: `esp_err_t lwlte_air780ep_init(const config, lwlte_t **out)`.
- Use `xxx_create()` / `xxx_destroy()` for independent opaque heap objects.
- Use `xxx_init(me, ...)` / `xxx_deinit(me)` for embedded objects, base classes, and composition members.
- Keep return semantics consistent: `init/deinit/destroy` return `esp_err_t`; `create` returns the object pointer.
- Ensure lifecycle functions are strictly paired: `create` cleans up with `destroy` on public ownership paths, and internally calls `deinit` before freeing; `init` failures unwind the successful initialization steps with matching `deinit` calls.

## Design

The implementation should apply the smallest semantic cleanup that preserves existing public API compatibility.

User-facing facade construction remains `lwlte_air780ep_init(config, out_lte)`. This function is a user API rather than a raw object factory, so it can allocate and assemble the full LTE dependency graph while returning `esp_err_t` and writing the output handle.

Independent opaque heap objects keep or gain this pattern:

```c
xxx_t *xxx_create(...);
esp_err_t xxx_destroy(xxx_t *me);
```

Internally, `xxx_create()` may allocate memory and call a private or layer-local `xxx_init(me, ...)`. If any step fails, it must unwind initialized resources through the matching `xxx_deinit(me)` path before freeing memory and returning `NULL`. `xxx_destroy()` must call `xxx_deinit(me)` before freeing the heap object.

Embedded objects, base classes, and composition members use this pattern:

```c
esp_err_t xxx_init(xxx_t *me, ...);
esp_err_t xxx_deinit(xxx_t *me);
```

These functions do not own allocation. They initialize or release fields inside memory owned by the caller.

## Scope

The main code target is the lifecycle boundary cleanup for existing modules, especially objects that currently combine allocation and initialization without an explicit internal `init/deinit` pair.

Expected module treatment:

- `lwlte_air780ep_init()` remains the user-facing initializer.
- `at_engine_create/destroy`, `core_create/destroy`, `mqtt_client_create/destroy`, `ping_client_create/destroy`, and concrete modem factories remain heap-object factories/destructors.
- `core` should gain an explicit internal `core_init(me, config, modem)` and `core_deinit(me)` pair if it lacks that boundary.
- `modem_base_init/deinit`, `core_fsm_init/deinit`, `net_mgr_init/deinit`, and `pdp_mgr_init` remain embedded/base/member initialization functions. If a matching `deinit` is missing for a member that owns resources, add it; if no resources are owned, no artificial no-op API is required unless needed for symmetry in cleanup code.

## Error Handling

`init` functions return the first initialization error. When a later initialization step fails, earlier successful steps are unwound in reverse order through matching `deinit` calls.

`destroy` and `deinit` return `esp_err_t`. When multiple cleanup steps can fail, the function should attempt all cleanup that is safe to attempt and return the first error seen. Memory must still be freed by `destroy` after deinit attempts to avoid leaks.

`create` returns `NULL` on allocation or initialization failure. Detailed failure reason remains available in logs, consistent with the existing factory style.

## Testing

Verification should include:

- Build the ESP-IDF project.
- Search lifecycle symbols to confirm naming consistency.
- Inspect cleanup paths for strict pairing between successful init steps and deinit steps.
- Confirm examples still call `lwlte_air780ep_init()` and `lwlte_destroy()` unchanged.

## Non-Goals

- Do not change `destroy/deinit` return types to `void`.
- Do not expose internal `init/deinit` APIs through public user headers unless they are already layer APIs.
- Do not convert the user-facing `lwlte_air780ep_init(config, out)` API into `create`.
- Do not introduce unrelated lifecycle abstractions or platform wrappers.
