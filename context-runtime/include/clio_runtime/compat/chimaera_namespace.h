// context-runtime/include/clio_runtime/compat/chimaera_namespace.h
//
// Module namespace migrated from `chimaera::` to `clio::run::`. This header
// keeps the legacy `chimaera::` form working for downstream code in two
// distinct ways, depending on what the downstream code is trying to do:
//
//  - Qualified name lookups like `chimaera::admin::Client` resolve to
//    `clio::run::admin::Client` via `using namespace` directives.
//
//  - New `namespace chimaera::<x> { class Foo; }` declarations remain
//    legal because `chimaera` is kept as a *real* namespace (not a
//    `namespace chimaera = clio_run;` alias, which would forbid opening
//    it as a namespace — see the build error in external/coeus-adapter
//    which declares `namespace chimaera::coeus_mdm { ... }`).
//
// External chimods that haven't migrated yet (coeus-adapter and friends)
// keep compiling unchanged.
//
// Auto-included by the umbrella <clio_runtime/clio_runtime.h>; you generally
// don't need to include it directly.
//
// See docs/deprecation-notes.md for the full migration table.

#ifndef CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_
#define CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_

// Forward-declare the canonical sub-namespaces so the `using namespace`
// directives below resolve even when the user only included a subset of
// the module headers.
namespace clio::run::admin {}
namespace clio::run::bdev {}
namespace clio::run::MOD_NAME {}

// Re-export each canonical sub-namespace under `chimaera::<x>`. `using
// namespace` makes every name declared in `clio::run::admin` (etc.)
// findable via `chimaera::admin::<name>` lookups, without freezing
// `chimaera::admin` as an alias — so new `namespace chimaera::other {}`
// declarations by downstream code remain legal.
namespace chimaera {
namespace admin     { using namespace clio::run::admin; }
namespace bdev      { using namespace clio::run::bdev; }
namespace MOD_NAME  { using namespace clio::run::MOD_NAME; }
}  // namespace chimaera

// Pre-`clio::run`-rename intermediate spelling.  In-tree code uses the
// canonical `clio::run::*` form everywhere now, but downstream code that
// already migrated off `chimaera::*` to the `clio_run::*` waypoint keeps
// compiling via this alias.  Safe to use the simple `namespace X = Y;`
// form here because no external chimod opens `namespace clio_run::xxx {}`
// (unlike `chimaera`, which coeus-adapter does open).
namespace clio_run = clio::run;

#endif  // CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_
