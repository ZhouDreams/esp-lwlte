# Opaque Handle Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the project's opaque object handle types from `xxx_t` to explicit `xxx_handle_t` names while keeping the existing explicit-pointer C style.

**Architecture:** This is a mechanical API/type rename, not a behavior change. The project will use `typedef struct xxx_handle xxx_handle_t;` and APIs will continue taking explicit pointers such as `lwlte_handle_t *me`, rather than hiding the pointer inside the typedef.

**Tech Stack:** C, ESP-IDF v6.0, FreeRTOS, current esp-lwlte component structure.

---

## Scope

Rename exactly these opaque object handles:

| Current type | New type | New struct tag |
|---|---|---|
| `lwlte_t` | `lwlte_handle_t` | `struct lwlte_handle` |
| `at_engine_t` | `at_engine_handle_t` | `struct at_engine_handle` |
| `modem_t` | `modem_handle_t` | `struct modem_handle` |
| `core_t` | `core_handle_t` | `struct core_handle` |
| `mqtt_client_t` | `mqtt_client_handle_t` | `struct mqtt_client_handle` |
| `ping_client_t` | `ping_client_handle_t` | `struct ping_client_handle` |

Do not rename these categories:

- Callback and handler types: `at_urc_handler_t`, `at_urc_callback_t`, `lwlte_event_callback_t`, `core_event_callback_t`, `mqtt_client_event_callback_t`, `modem_event_callback_t`.
- Config/value objects: `lwlte_air780ep_config_t`, `at_engine_config_t`, `at_response_t`, `modem_info_t`, `core_cmd_t`, `mqtt_client_config_t`, `ping_client_request_t`, etc.
- Private concrete modem subclasses: `modem_air780ep_t`, `modem_ml307r_t`.
- Internal component/value types: `core_fsm_t`, `net_mgr_t`, `pdp_mgr_t`, `mqtt_pending_cmd_t`, `ping_wait_ctx_t`, `air780ep_cmd_ctx_t`, `ml307r_cmd_ctx_t`.

No backward-compatible aliases should be added unless external compatibility is explicitly required. The project is still under active internal development, and adding `typedef lwlte_handle_t lwlte_t;` style aliases would keep the old naming alive.

---

## Files To Modify

Public/layer headers:

- `src/include/lwlte.h`
- `src/at_engine/at_engine.h`
- `src/modem/modem.h`
- `src/modem/modem_priv.h`
- `src/modem/modem_air780ep.h`
- `src/modem/modem_ml307r.h`
- `src/core/core.h`
- `src/core/core_priv.h`
- `src/mqtt_client/mqtt_client.h`
- `src/mqtt_client/mqtt_client_priv.h`
- `src/ping_client/ping_client.h`
- `src/ping_client/ping_client_priv.h`
- `src/lwlte/lwlte_priv.h`

Source files:

- `src/at_engine/at_engine.c`
- `src/modem/modem.c`
- `src/modem/modem_air780ep.c`
- `src/modem/modem_ml307r.c`
- `src/core/core.c`
- `src/core/core_fsm.c`
- `src/core/net_mgr.c`
- `src/core/pdp_mgr.c`
- `src/mqtt_client/mqtt_client.c`
- `src/ping_client/ping_client.c`
- `src/lwlte/lwlte.c`
- `src/lwlte/lwlte_air780ep.c`
- `src/lwlte/lwlte_ml307r.c`

Examples:

- `example/air780ep_basic_connect.c`
- `example/air780ep_mqtt_client.c`
- `example/ml307r_basic_connect.c`
- `example/ml307r_mqtt_client.c`

Active agent docs:

- `docs/agents/classes.md`
- `docs/agents/architecture.md`
- `docs/agents/directory-structure.md`
- `docs/agents/oop-design.md`
- `docs/agents/err.md` if it contains examples using old handle names.

Historical `docs/superpowers/specs/` and `docs/superpowers/plans/` files are records of past work and should not be rewritten unless the explicit goal is full-repository textual cleanup.

---

### Task 1: Rename Public Opaque Typedefs And API Signatures

**Files:**

- Modify: `src/include/lwlte.h`
- Modify: `src/at_engine/at_engine.h`
- Modify: `src/modem/modem.h`
- Modify: `src/modem/modem_air780ep.h`
- Modify: `src/modem/modem_ml307r.h`
- Modify: `src/core/core.h`
- Modify: `src/mqtt_client/mqtt_client.h`
- Modify: `src/ping_client/ping_client.h`

- [ ] **Step 1: Change the six opaque declarations**

Use these exact declarations:

```c
typedef struct lwlte_handle lwlte_handle_t;
typedef struct at_engine_handle at_engine_handle_t;
typedef struct modem_handle modem_handle_t;
typedef struct core_handle core_handle_t;
typedef struct mqtt_client_handle mqtt_client_handle_t;
typedef struct ping_client_handle ping_client_handle_t;
```

- [ ] **Step 2: Change public API signatures that reference the old types**

Examples of the target style:

```c
esp_err_t lwlte_air780ep_init(const lwlte_air780ep_config_t *config,
                              lwlte_handle_t **out_lte);
esp_err_t lwlte_destroy(lwlte_handle_t *me);

at_engine_handle_t *at_engine_create(const at_engine_config_t *config);
esp_err_t at_engine_destroy(at_engine_handle_t *me);

modem_handle_t *modem_air780ep_create(at_engine_handle_t *at,
                                      const modem_air780ep_config_t *config);

core_handle_t *core_create(const core_config_t *config,
                           modem_handle_t *modem);

mqtt_client_handle_t *mqtt_client_create(const mqtt_client_config_t *config,
                                         core_handle_t *core);

ping_client_handle_t *ping_client_create(core_handle_t *core);
```

- [ ] **Step 3: Update callback signatures only where they take object handles**

Do not rename the callback type names themselves. Only update their parameters:

```c
typedef void (*lwlte_event_callback_t)(lwlte_handle_t *lte,
                                       lwlte_event_id_t event_id,
                                       const lwlte_event_data_t *data,
                                       void *user_ctx);

typedef void (*core_event_callback_t)(core_handle_t *core,
                                      core_event_id_t event_id,
                                      const core_event_data_t *data,
                                      void *user_ctx);

typedef void (*modem_event_callback_t)(modem_handle_t *modem,
                                       const modem_event_t *event,
                                       void *user_ctx);

typedef void (*mqtt_client_event_callback_t)(mqtt_client_handle_t *client,
                                             mqtt_client_event_id_t event_id,
                                             const mqtt_client_event_data_t *data,
                                             void *user_ctx);
```

- [ ] **Step 4: Search public headers for stale old handle type names**

Run:

```bash
rg '\b(lwlte_t|at_engine_t|modem_t|core_t|mqtt_client_t|ping_client_t)\b' src/include src/at_engine src/modem src/core src/mqtt_client src/ping_client -g '*.h'
```

Expected: no matches for the six old opaque handle type names in active headers.

---

### Task 2: Rename Internal Struct Tags And Source References

**Files:**

- Modify: all source and private header files listed in “Files To Modify”.

- [ ] **Step 1: Rename internal struct definitions**

Use these exact tag names:

```c
struct lwlte_handle {
    /* existing fields unchanged */
};

struct at_engine_handle {
    /* existing fields unchanged */
};

struct modem_handle {
    /* existing fields unchanged */
};

struct core_handle {
    /* existing fields unchanged */
};

struct mqtt_client_handle {
    /* existing fields unchanged */
};

struct ping_client_handle {
    /* existing fields unchanged */
};
```

- [ ] **Step 2: Rename all internal references to the six old type names**

Apply word-boundary replacements in active code only:

```text
lwlte_t        -> lwlte_handle_t
at_engine_t    -> at_engine_handle_t
modem_t        -> modem_handle_t
core_t         -> core_handle_t
mqtt_client_t  -> mqtt_client_handle_t
ping_client_t  -> ping_client_handle_t
```

- [ ] **Step 3: Update modem subclass base fields and helpers**

Target style:

```c
typedef struct {
    modem_handle_t base;
    modem_air780ep_config_t config;
    /* existing fields unchanged */
} modem_air780ep_t;

static modem_air780ep_t *to_air780ep(modem_handle_t *me)
{
    return MODEM_CONTAINER_OF(me, modem_air780ep_t, base);
}
```

Apply the same pattern to `modem_ml307r_t`.

- [ ] **Step 4: Fix allocation expressions that name the old typedef directly**

Target style:

```c
at_engine_handle_t *me = calloc(1, sizeof(*me));
```

Prefer `sizeof(*me)` over `sizeof(at_engine_handle_t)` so future type renames do not affect allocation correctness.

- [ ] **Step 5: Search active source for stale old handle type names**

Run:

```bash
rg '\b(lwlte_t|at_engine_t|modem_t|core_t|mqtt_client_t|ping_client_t)\b' src example -g '*.{h,c}'
```

Expected: no matches.

---

### Task 3: Update Examples And Active Architecture Docs

**Files:**

- Modify: `example/air780ep_basic_connect.c`
- Modify: `example/air780ep_mqtt_client.c`
- Modify: `example/ml307r_basic_connect.c`
- Modify: `example/ml307r_mqtt_client.c`
- Modify: `docs/agents/classes.md`
- Modify: `docs/agents/architecture.md`
- Modify: `docs/agents/directory-structure.md`
- Modify: `docs/agents/oop-design.md`
- Modify: `docs/agents/err.md` if needed.

- [ ] **Step 1: Update example handle variables and callbacks**

Target style:

```c
static void lte_event_cb(lwlte_handle_t *lte,
                         lwlte_event_id_t event_id,
                         const lwlte_event_data_t *data,
                         void *user_ctx);

lwlte_handle_t *lte = NULL;
```

- [ ] **Step 2: Update active agent docs to match the new canonical naming**

Replace references in active docs:

```text
lwlte_t        -> lwlte_handle_t
at_engine_t    -> at_engine_handle_t
modem_t        -> modem_handle_t
core_t         -> core_handle_t
mqtt_client_t  -> mqtt_client_handle_t
ping_client_t  -> ping_client_handle_t
```

Also update prose from “`xxx_t` opaque handle” to “`xxx_handle_t` opaque handle”.

- [ ] **Step 3: Do not rewrite historical plans/specs by default**

Leave files under `docs/superpowers/specs/` and older `docs/superpowers/plans/` unchanged unless the requested acceptance criterion is full-repository grep cleanliness.

---

### Task 4: Verification

**Files:**

- No direct edits.

- [ ] **Step 1: Verify active code has no stale old handle types**

Run:

```bash
rg '\b(lwlte_t|at_engine_t|modem_t|core_t|mqtt_client_t|ping_client_t)\b' src example -g '*.{h,c}'
```

Expected: no output.

- [ ] **Step 2: Verify active docs have no stale canonical references**

Run:

```bash
rg '\b(lwlte_t|at_engine_t|modem_t|core_t|mqtt_client_t|ping_client_t)\b' docs/agents -g '*.md'
```

Expected: no output, except if a doc intentionally contrasts old and new names in a migration note.

- [ ] **Step 3: Build through the ESP-IDF MCP tool**

Run the project build using the available ESP-IDF build tool.

Expected: build succeeds with no type-name errors.

- [ ] **Step 4: If MCP build is unavailable, build through IDF shell**

Run:

```bash
source ~/.espressif/v6.0/esp-idf/export.sh && idf.py build
```

Expected: build succeeds.

---

## Review Notes

- This plan intentionally keeps explicit pointer syntax: `lwlte_handle_t *lte`. This differs from the common ESP-IDF pattern `typedef struct esp_timer *esp_timer_handle_t`, but matches the requested “original basis” style and avoids hiding pointer semantics inside typedefs.
- This plan intentionally does not add compatibility aliases from old names to new names. Add aliases only if existing external application code must keep compiling during a transition window.
- The change is source-breaking for users because `lwlte_t *` becomes `lwlte_handle_t *` in public API signatures.
