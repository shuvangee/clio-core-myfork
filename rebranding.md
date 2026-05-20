# Rebranding: `chimaera` → `clio_runtime`, `hermes_shm` → `clio_ctp`

This document describes the public-API renames from `chimaera` to
`clio_runtime` (the task-execution runtime) and from `hermes_shm` /
`HSHM` / `hshm::` to `clio_ctp` / `CTP_` / `ctp::` (the lower-level
shared-memory / transport primitives), and what external projects need
to do (if anything) to migrate.

**TL;DR — nothing in your code needs to change immediately.** Every legacy
identifier, include path, env variable, and config-file name remains a working
alias of its new counterpart. You can migrate at your own pace, file by file.

See the [Migration table](#migration-table) below for the mapping. The
[Backward-compat shims](#backward-compat-shims) section explains the
mechanisms that keep old code working.

---

## Migration table

### Runtime layer (`chimaera` → `clio_runtime`)

| Area | Old (still works) | New (preferred) | Compat mechanism |
| --- | --- | --- | --- |
| Header directory | `include/chimaera/…` | `include/clio_runtime/…` | Forwarder shim headers at the old path `#include` the new path. |
| Main umbrella header | `<chimaera/chimaera.h>` | `<clio_runtime/clio_runtime.h>` | Same. |
| Init function | `chi::CHIMAERA_INIT(...)` | `chi::CLIO_RUNTIME_INIT(...)` | `#define CLIO_RUNTIME_INIT chi::CHIMAERA_INIT` |
| Finalize function | `chi::CHIMAERA_FINALIZE()` | `chi::CLIO_RUNTIME_FINALIZE()` | `#define CLIO_RUNTIME_FINALIZE chi::CHIMAERA_FINALIZE` |
| Init-mode enum | `chi::ChimaeraMode` | `chi::ChimaeraMode` *(unchanged for now)* | — |
| Singleton accessors | `CHI_IPC`, `CHI_ADMIN`, `CHI_POOL_MANAGER`, … | `CLIO_IPC`, `CLIO_ADMIN`, `CLIO_POOL_MANAGER`, … | `#define CLIO_X CHI_X` |
| Container-class macros | `CHI_CHIMOD_CC(...)`, `CHI_TASK_CC(...)` | `CLIO_CHIMOD_CC(...)`, `CLIO_TASK_CC(...)` | Same. |
| Coroutine task body | `CHI_TASK_BODY_BEGIN`/`END`, `CHI_CO_AWAIT`, `CHI_CO_RETURN` | `CLIO_TASK_BODY_BEGIN`/`END`, `CLIO_CO_AWAIT`, `CLIO_CO_RETURN` | Same. |
| Allocator macros | `CHI_QUEUE_ALLOC_T`, `CHI_TASK_ALLOC_T`, `CHI_PRIV_ALLOC[_T]`, `CHI_PRIV_SHARED_ALLOC[_T]` | `CLIO_QUEUE_ALLOC_T`, `CLIO_TASK_ALLOC_T`, `CLIO_PRIV_ALLOC[_T]`, `CLIO_PRIV_SHARED_ALLOC[_T]` | Same. |
| Env variables | `CHI_WITH_RUNTIME`, `CHI_IPC_MODE`, `CHI_SERVER_CONF`, `CHI_REPO_PATH`, `CHI_NUM_CONTAINERS`, `CHI_GPU_BLOCKS`, `CHI_GPU_THREADS`, `CHI_INIT_ATTEMPTS`, `CHI_INIT_SLEEP_MS`, `CHI_INIT_STAGGER_MS`, `CHI_CLIENT_RETRY_TIMEOUT`, `CHI_CLIENT_TRY_NEW_SERVERS`, `CHI_LBM_THALLIUM_PROTOCOL`, `CHI_LBM_THALLIUM_RPC_THREADS`, `CHI_LBM_ZMQ_STATS`, `CHI_MEMFD_DIR`, `CHI_TEST_DATA_DIR`, `CHI_WAIT_SERVER`, `CHI_ZMQ_IO_THREADS` | Same names with `CLIO_` prefix instead of `CHI_` | Every runtime `getenv` was rewritten to `ctp::env::GetCompat("…")`, which reads `CLIO_<suffix>` first and falls back to `CHI_<suffix>`. Setting either env variable works. |
| CMake project options | `WRP_CORE_ENABLE_*` | `CLIO_CORE_ENABLE_*` | Renamed in the prior `WRP_ → CLIO_` pass; no compat alias for these CMake-level variables. |
| Repo metadata YAML | `chimaera_repo.yaml` | `clio_repo.yaml` | The CMake repo-parser checks for `clio_repo.yaml` first, then falls back to `chimaera_repo.yaml`. Either filename is accepted. |
| Module metadata YAML | `chimaera_mod.yaml` | `clio_mod.yaml` | Same — parser checks `clio_mod.yaml` first, falls back to `chimaera_mod.yaml`. |
| Library names (`.so`/CMake targets) | `libchimaera_cxx.so`, `chimaera_cxx`, `libchimaera_admin_runtime.so`, … | unchanged | Not renamed in this pass. |
| Daemon binary | `chimaera runtime start` | unchanged | Not renamed in this pass. |
| C++ namespace | `chi::…` | unchanged | Not renamed in this pass. |

### Transport-primitives layer (`hermes_shm` / `HSHM` / `hshm::` → `clio_ctp` / `CTP_` / `ctp::`)

| Area | Old (still works) | New (preferred) | Compat mechanism |
| --- | --- | --- | --- |
| Header directory | `<hermes_shm/…>` | `<clio_ctp/…>` | 113 forwarder shim headers at `include/hermes_shm/…` that `#include <clio_ctp/…>`. |
| Umbrella header | `<hermes_shm/hermes_shm.h>` | `<clio_ctp/clio_ctp.h>` | Same. |
| Top-level namespace | `hshm::` | `ctp::` | `namespace hshm = ctp;` in `clio_ctp/compat/hshm_aliases.h` (re-exported via the umbrella). |
| IPC sub-namespace | `hshm::ipc::` *and* the historical short alias `hipc::` | `ctp::ipc::` | `namespace hshm = ctp;` (transitive) and the explicit shorthand `namespace hipc = ctp::ipc;` — both work. |
| Sub-namespaces | `hshm::thread::`, `hshm::lbm::`, `hshm::ipc::`, … | `ctp::thread::`, `ctp::lbm::`, `ctp::ipc::`, … | Resolved transitively via `namespace hshm = ctp`. |
| Function/type macros | `HSHM_CROSS_FUN`, `HSHM_INLINE`, `HSHM_GPU_FUN`, `HSHM_MALLOC`, `HSHM_ROOT_ALLOC`, `HSHM_DEFAULT_ALLOC`, `HSHM_DEFAULT_ALLOC_GPU_T`, `HSHM_DEVICE_*`, `HSHM_DLL*`, `HSHM_GET_GLOBAL_*`, `HSHM_DEFINE_GLOBAL_*`, `HSHM_ENABLE_*`, `HSHM_ERROR_*`, `HSHM_LOG`, `HSHM_LOG_LEVEL`, `HSHM_SYSTEM_INFO`, `HSHM_THREAD_MODEL`, `HSHM_THROW_ERROR`, `HSHM_IS_*`, … (89 in total) | Same names with `CTP_` prefix | One `#define HSHM_X CTP_X` per macro in `clio_ctp/compat/hshm_aliases.h`. Generated mechanically by scanning every `#define CTP_*` in the tree (include-guard-style names excluded). |
| Inclusion path | — | `<clio_ctp/clio_ctp.h>` auto-includes the alias header at the bottom. | Any TU that pulls in the umbrella sees the aliases. |

---

## Backward-compat shims

### Header forwarder shims

The directory `context-runtime/include/chimaera/` no longer holds canonical
headers — those moved to `context-runtime/include/clio_runtime/`. In its place
sits a parallel tree of one-line forwarder headers, e.g.:

```cpp
// context-runtime/include/chimaera/types.h
// Backward-compat forwarding shim.
// Prefer the new path: <clio_runtime/types.h>.
#include <clio_runtime/types.h>
```

The same applies under `context-runtime/modules/<mod>/include/chimaera/` →
`…/clio_runtime/`. Every header is reachable via either include path.

### Macro and function aliases

All `CLIO_*` macros are `#define`d to the existing `CHI_*` macros (or
`::chi::CHIMAERA_INIT` etc. for the init functions). Aliases are placed in
`<clio_runtime/clio_runtime.h>` *after* the includes that define the
`CHI_*` macros, so a TU that already pulled in any `chimaera_*` header will
automatically see the `CLIO_*` aliases too.

### Env variable graceful resolution

A small inline helper `ctp::env::GetCompat(const char* suffix)` lives in
`<clio_ctp/util/env_compat.h>` and is re-exposed as `chi::env::GetCompat`
in `<clio_runtime/types.h>`. The helper:

1. Looks up `CLIO_<suffix>` in the process environment.
2. If not set, looks up `CHI_<suffix>`.
3. Returns `nullptr` if neither is set.

Every runtime `std::getenv("CHI_…")` call site was rewritten to use this
helper. Existing deployments that set `CHI_*` env vars continue to work
unchanged; new deployments should prefer `CLIO_*`.

### Repo / module YAML files

The CMake helpers `read_repo_namespace` and `chimaera_read_module_config`
search `clio_repo.yaml` / `clio_mod.yaml` first, then fall back to
`chimaera_repo.yaml` / `chimaera_mod.yaml`. Either filename works; the new
name takes precedence if both exist.

The IOWarp Core repository itself migrated to the new names; external
chimods can rename at their leisure.

---

## What's *not* renamed (yet)

- The `chi::` C++ namespace.
- CMake target names (`chimaera_cxx`, `chimaera_admin_runtime`, …) and their
  installed shared-library filenames.
- The `chimaera` daemon binary (`chimaera runtime start` is still the
  invocation).
- The `ChimaeraMode` enum class.
- Identifiers in code that include `chimaera` as a fragment (e.g., the
  `Chimaera` class itself, `chi::Chimaera`).

These were left alone to keep the public ABI surface stable in this pass and
to avoid breaking dynamic-loader paths and shell pipelines. They can be done
in follow-up passes with a similar shim/alias strategy if desired.

---

## Migrating your code

If you maintain a downstream project, **you don't have to do anything** —
the shims and aliases keep your existing code building and running. When
you're ready to switch to the new names, the mechanical migration is:

```bash
# In your project root:
grep -rl '<chimaera/' --include='*.cc' --include='*.h' --include='*.cpp' \
  | xargs sed -i -E 's|<chimaera/|<clio_runtime/|g; s|<clio_runtime/chimaera\.h>|<clio_runtime/clio_runtime.h>|g'

grep -rl '"chimaera/' --include='*.cc' --include='*.h' --include='*.cpp' \
  | xargs sed -i -E 's|"chimaera/|"clio_runtime/|g; s|"clio_runtime/chimaera\.h"|"clio_runtime/clio_runtime.h"|g'

# Macros:
grep -rl 'CHIMAERA_INIT\|CHIMAERA_FINALIZE\|CHI_ADMIN\|CHI_IPC\|CHI_POOL_MANAGER\|CHI_CONFIG_MANAGER\|CHI_MODULE_MANAGER\|CHI_WORK_ORCHESTRATOR\|CHI_CUR_WORKER\|CHI_TASK_BODY_\|CHI_CO_\|CHI_CHIMOD_CC\|CHI_TASK_CC\|CHI_QUEUE_ALLOC_T\|CHI_TASK_ALLOC_T\|CHI_PRIV_' \
  --include='*.cc' --include='*.h' --include='*.cpp' \
  | xargs sed -i -E \
    -e 's/\bCHIMAERA_INIT\b/CLIO_RUNTIME_INIT/g' \
    -e 's/\bCHIMAERA_FINALIZE\b/CLIO_RUNTIME_FINALIZE/g' \
    -e 's/\bCHI_(ADMIN|IPC|CPU_IPC|POOL_MANAGER|CONFIG_MANAGER|MODULE_MANAGER|WORK_ORCHESTRATOR|CUR_WORKER|CHIMOD_CC|TASK_CC|TASK_BODY_BEGIN|TASK_BODY_END|CO_AWAIT|CO_RETURN|QUEUE_ALLOC_T|TASK_ALLOC_T|PRIV_ALLOC_T|PRIV_ALLOC|PRIV_SHARED_ALLOC_T|PRIV_SHARED_ALLOC|CHIMAERA_MANAGER)\b/CLIO_\1/g'

# Env variables in your launch scripts: just rename CHI_X to CLIO_X.

# Config YAMLs: rename chimaera_repo.yaml -> clio_repo.yaml and
# chimaera_mod.yaml -> clio_mod.yaml in place. No content changes needed.
```

After migrating, your project will work against this version of IOWarp Core
and any future version that keeps the compat shims in place.

---

## Removal timeline for the compat shims

No removal is planned at this time. The shims, aliases, and dual-file-name
parsers are intended to be permanent until an explicit deprecation is
announced, at which point external consumers will get a clear heads-up.
