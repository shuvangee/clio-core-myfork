#ifndef CLIO__CAE_CODE_INDEXER_AUTOGEN_METHODS_H_
#define CLIO__CAE_CODE_INDEXER_AUTOGEN_METHODS_H_

#include <clio_runtime/clio_runtime.h>
#include <string>
#include <vector>

/**
 * Auto-generated method definitions for code_indexer
 */

namespace clio::cae::code_indexer {

namespace Method {
// Inherited methods
GLOBAL_CROSS_CONST clio::run::u32 kCreate = 0;
GLOBAL_CROSS_CONST clio::run::u32 kDestroy = 1;
GLOBAL_CROSS_CONST clio::run::u32 kMonitor = 9;

// code_indexer-specific methods
GLOBAL_CROSS_CONST clio::run::u32 kIndexPath = 10;
GLOBAL_CROSS_CONST clio::run::u32 kQuery = 11;
GLOBAL_CROSS_CONST clio::run::u32 kStatus = 12;

GLOBAL_CROSS_CONST clio::run::u32 kMaxMethodId = 13;

inline const std::vector<std::string>& GetMethodNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v(kMaxMethodId);
    v[0] = "Create";
    v[1] = "Destroy";
    v[9] = "Monitor";
    v[10] = "IndexPath";
    v[11] = "Query";
    v[12] = "Status";
    return v;
  }();
  return names;
}
}  // namespace Method

}  // namespace clio::cae::code_indexer

#endif  // CODE_INDEXER_AUTOGEN_METHODS_H_
