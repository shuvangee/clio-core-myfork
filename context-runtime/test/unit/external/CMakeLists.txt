cmake_minimum_required(VERSION 3.20)
project(external_chimod_test)

# Set C++ standard
set(CMAKE_CXX_STANDARD_20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Unified IOWarp Core package — `clio-core` is the canonical name; the
# legacy `iowarp-core` spelling still resolves to the same Config.
find_package(clio-core CONFIG REQUIRED)
# This automatically provides:
#   - All ctp::* modular targets (ctp::cxx, ctp::configure, ctp::serialize, etc.)
#   - clio::run::cxx (core runtime library)
#   - clio::run::admin_client and clio::run::admin_runtime
#   - ChiMod build utilities

# Find system dependencies
find_package(Threads REQUIRED)

# Create test executable
add_executable(test_external_chimod test_external_chimod.cc)

# Link against required libraries
target_link_libraries(test_external_chimod
  clio::run::admin_client                 # Admin client (required, includes clio::run::cxx)
  ${CMAKE_THREAD_LIBS_INIT}               # Threading support
)

message(STATUS "External ChiMod test configured successfully")
message(STATUS "  Target: test_external_chimod")
message(STATUS "  Links to: clio::run::admin_client (includes clio::run::cxx automatically)")

# Enable testing
enable_testing()
add_test(NAME external_chimod_test COMMAND test_external_chimod)
