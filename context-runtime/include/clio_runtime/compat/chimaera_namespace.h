// context-runtime/include/clio_runtime/compat/chimaera_namespace.h
//
// Module namespace migrated from `chimaera::` to `clio_run::`. This header
// declares the alias `namespace chimaera = clio_run;` so legacy code that
// uses qualified names like `chimaera::admin::Client` or
// `chimaera::bdev::Runtime` keeps compiling unchanged.
//
// Auto-included by the umbrella <clio_runtime/clio_runtime.h>; you generally
// don't need to include it directly.
//
// See docs/deprecation-notes.md for the full migration table.

#ifndef CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_
#define CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_

namespace clio_run {}  // forward-declare so the alias below resolves

namespace chimaera = clio_run;  // legacy alias

#endif  // CLIO_RUNTIME_COMPAT_CHIMAERA_NAMESPACE_H_
