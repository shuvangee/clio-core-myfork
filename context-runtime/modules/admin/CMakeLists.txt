cmake_minimum_required(VERSION 3.10)


# Create client library
# LIB_NAME pins the library to `chimaera_admin_*` so changing the
# C++ namespace (chimaera -> clio_run) doesn't cascade into renaming
# this load-bearing .so. See deprecation-notes.md.
add_chimod_client(
  LIB_NAME chimaera_admin
  SOURCES
    src/admin_client.cc
)

# Create runtime library (see client block above for LIB_NAME rationale).
add_chimod_runtime(
  LIB_NAME chimaera_admin
  SOURCES
    src/admin_runtime.cc
    src/autogen/admin_lib_exec.cc
)

# Add unit tests subdirectory
if(CLIO_CORE_ENABLE_TESTS)
  add_subdirectory(test)
endif()