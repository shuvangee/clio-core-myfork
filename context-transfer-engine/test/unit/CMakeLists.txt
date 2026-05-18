# ------------------------------------------------------------------------------
# CTE Core Unit Tests
# ------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.10)

# Note: Binary output directory is configured in root CMakeLists.txt to use build/bin
# The test executable will be placed in build/bin directory due to global
# CMAKE_RUNTIME_OUTPUT_DIRECTORY setting

# Find required test dependencies
# Catch2 is already found by root CMakeLists.txt

# Create the test executable
add_executable(cte_core_unit_tests
    test_core_minimal.cc
    test_core_functionality_simple.cc
)

# Create separate executable for config/DPE tests (avoids TEST_CASE line number collisions)
add_executable(test_cte_config_dpe
    test_cte_config_dpe.cc
)

# Create single version of test_core_functionality that uses environment variable
# CHI_WITH_RUNTIME to control whether runtime is initialized (default: yes)
add_executable(test_core_functionality
    test_core_functionality.cc
)

# Create test_query executable for query API tests (TagQuery, BlobQuery)
add_executable(test_query
    test_query.cc
)

# Create test_tag_operations executable for Tag API coverage tests
add_executable(test_tag_operations
    test_tag_operations.cc
)

# Create test_core_client_config executable for Client and Config API coverage tests
add_executable(test_core_client_config
    test_core_client_config.cc
)

# Create test_core_runtime_coverage executable for Runtime method coverage tests
add_executable(test_core_runtime_coverage
    test_core_runtime_coverage.cc
)

# Create test_tiered_storage_stress executable for tiered storage stress tests
add_executable(test_tiered_storage_stress
    test_tiered_storage_stress.cc
)

# Create test_reorganize_blob executable for ReorganizeBlob API tests
add_executable(test_reorganize_blob
    test_reorganize_blob.cc
)

# GPU-runtime-era CUDA tests were removed when the GPU runtime concept
# was retired. The kernel→CPU submission path is now exercised by:
#   - context-runtime/test/unit/gpu/test_gpu_kernel_stress_*
#     (kPinnedHost task submission to a dummy chimod).
#   - context-transfer-engine/adapter/gpu_vector/test/test_gpu_vector_gpu.cc
#     (kDeviceMem-backed pages flushed to bdev kRam via PutBlob).

# Include directories for all tests - using unified simple_test.h framework
target_include_directories(cte_core_unit_tests PRIVATE

)

target_include_directories(test_core_functionality PRIVATE

)

target_include_directories(test_query PRIVATE

)

target_include_directories(test_cte_config_dpe PRIVATE

)

target_include_directories(test_tag_operations PRIVATE

)

target_include_directories(test_core_client_config PRIVATE

)

target_include_directories(test_core_runtime_coverage PRIVATE

)

target_include_directories(test_tiered_storage_stress PRIVATE

)

target_include_directories(test_reorganize_blob PRIVATE

)

# GPU runtime tests + their include directories removed.

# Link against required libraries using simple_test.h framework (NOT Catch2)
target_link_libraries(cte_core_unit_tests
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_core_functionality - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_core_functionality
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_cte_config_dpe - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_cte_config_dpe
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_query - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_query
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_tag_operations - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_tag_operations
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_core_client_config - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_core_client_config
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_core_runtime_coverage - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_core_runtime_coverage
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_tiered_storage_stress - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_tiered_storage_stress
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# Link test_reorganize_blob - using simple_test.h framework (NOT Catch2)
target_link_libraries(test_reorganize_blob
    wrp_cte_core_runtime         # CTE core runtime library
    wrp_cte_core_client          # CTE core client library
    chimaera::admin_runtime      # Admin module runtime (required for runtime mode)
    chimaera::admin_client       # Admin module client
    chimaera::bdev_runtime       # Bdev module runtime (required for runtime mode)
    chimaera::bdev_client        # Bdev client for BdevType enum
    hshm::cxx                    # HermesShm library
    ${CMAKE_THREAD_LIBS_INIT}    # Threading support
)

# GPU-runtime-era link rules removed.

# Note: Include directories are provided automatically via linked ChiMod targets
# wrp_cte_core_client provides ${WRP_CTE_ROOT}/core/include via transitive includes

# Manual test registration for specific test cases
add_test(NAME cte_core_pool_creation
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_target_registration
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_blob_put
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_blob_get
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_integration
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_client_creation
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_params
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_target_config
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_tag_info
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_blob_info
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_tasks
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_helpers
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_workflow
    COMMAND cte_core_unit_tests)

add_test(NAME cte_core_performance
    COMMAND cte_core_unit_tests)

# CTE Config and DPE tests (separate executable)
add_test(NAME cte_config_tests
    COMMAND test_cte_config_dpe "[cte][config]")
add_test(NAME cte_dpe_tests
    COMMAND test_cte_config_dpe "[cte][dpe]")

# Add test_core_functionality as a single test (the binary runs all 9 internal
# test cases in one invocation; duplicating CTest entries causes parallel runs
# that fight over shared memory and ports).
add_test(NAME cte_functional_all
    COMMAND test_core_functionality)

# Add test_query tests - use test names for filtering (simple_test.h filters by name, not tags)
add_test(NAME cte_query_tag_exact
    COMMAND test_query "TagQuery - Exact Match")

add_test(NAME cte_query_tag_wildcard
    COMMAND test_query "TagQuery - Wildcard Pattern")

add_test(NAME cte_query_tag_alternation
    COMMAND test_query "TagQuery - Alternation Pattern")

add_test(NAME cte_query_tag_matchall
    COMMAND test_query "TagQuery - Match All Pattern")

add_test(NAME cte_query_tag_nomatch
    COMMAND test_query "TagQuery - No Matches")

add_test(NAME cte_query_blob_exact
    COMMAND test_query "BlobQuery - Exact Match")

add_test(NAME cte_query_blob_wildcard
    COMMAND test_query "BlobQuery - Wildcard Patterns")

add_test(NAME cte_query_blob_multitag
    COMMAND test_query "BlobQuery - Multiple Tags")

add_test(NAME cte_query_blob_matchall
    COMMAND test_query "BlobQuery - Match All")

add_test(NAME cte_query_blob_no_blob
    COMMAND test_query "BlobQuery - No Blob Matches")

add_test(NAME cte_query_blob_no_tag
    COMMAND test_query "BlobQuery - No Tag Matches")

# Note: cte_query_blob_extension test removed - not implemented in simple_test.h version

add_test(NAME cte_query_local_poolquery
    COMMAND test_query "Query - Local Pool Query")

# Add test_tag_operations tests - comprehensive Tag API coverage tests
add_test(NAME cte_tag_construction
    COMMAND test_tag_operations "Tag - Construction")

add_test(NAME cte_tag_putblob
    COMMAND test_tag_operations "Tag - PutBlob")

add_test(NAME cte_tag_getblob
    COMMAND test_tag_operations "Tag - GetBlob")

add_test(NAME cte_tag_errors
    COMMAND test_tag_operations "Tag - Error")

add_test(NAME cte_tag_metadata
    COMMAND test_tag_operations "Tag - Metadata")

add_test(NAME cte_tag_reorganize
    COMMAND test_tag_operations "Tag - Reorganize")

add_test(NAME cte_tag_async
    COMMAND test_tag_operations "Tag - Async")

add_test(NAME cte_tag_large
    COMMAND test_tag_operations "Tag - Large")

add_test(NAME cte_tag_edge
    COMMAND test_tag_operations "Tag - Edge")

# Single-process runs of test_core_client_config / test_core_runtime_coverage.
# Previously each Catch2 SECTION was registered as a standalone ctest entry
# (27 + 19), which serialized via RESOURCE_LOCK but multiplied chimaera
# init/finalize cycles and exposed shutdown ZMQ races.
add_test(NAME cte_client_config_all
    COMMAND test_core_client_config)

add_test(NAME cte_runtime_coverage_all
    COMMAND test_core_runtime_coverage)

# Set test properties for proper execution environment
set_tests_properties(
    cte_core_pool_creation
    cte_core_target_registration
    cte_core_blob_put
    cte_core_blob_get
    cte_core_integration
    cte_core_client_creation
    cte_core_params
    cte_core_target_config
    cte_core_tag_info
    cte_core_blob_info
    cte_core_tasks
    cte_core_helpers
    cte_config_tests
    cte_dpe_tests
    cte_core_workflow
    cte_core_performance
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout for each test
        LABELS "unit;core;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
)

set_tests_properties(
    cte_functional_all
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout
        LABELS "functional;core;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

set_tests_properties(
    cte_query_tag_exact
    cte_query_tag_wildcard
    cte_query_tag_alternation
    cte_query_tag_matchall
    cte_query_tag_nomatch
    cte_query_blob_exact
    cte_query_blob_wildcard
    cte_query_blob_multitag
    cte_query_blob_matchall
    cte_query_blob_no_blob
    cte_query_blob_no_tag
    cte_query_local_poolquery
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout for each test
        LABELS "query;core;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

set_tests_properties(
    cte_tag_construction
    cte_tag_putblob
    cte_tag_getblob
    cte_tag_errors
    cte_tag_metadata
    cte_tag_reorganize
    cte_tag_async
    cte_tag_large
    cte_tag_edge
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout for each test
        LABELS "unit;tag;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

set_tests_properties(cte_client_config_all PROPERTIES
    TIMEOUT 600
    LABELS "unit;core;cte;client;config"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
)

set_tests_properties(cte_runtime_coverage_all PROPERTIES
    TIMEOUT 600
    LABELS "unit;core;cte;runtime;coverage"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

# Add tiered storage stress tests
# NOTE: These tests are chained (Put -> Reorganize -> Integrity -> Cleanup)
# and share global state via g_fixture, so they must run together in a single
# process. Running them as separate ctest entries would lose shared state.
add_test(NAME cte_tiered_storage_all
    COMMAND test_tiered_storage_stress)

set_tests_properties(
    cte_tiered_storage_all
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout
        LABELS "stress;tiered;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

# Add reorganize blob tests
# NOTE: These tests are chained (Put -> Move -> Integrity -> Promote -> Cleanup)
# and share global state via g_fixture, so they must run together in a single
# process. Running them as separate ctest entries would lose shared state.
add_test(NAME cte_reorganize_all
    COMMAND test_reorganize_blob)

set_tests_properties(
    cte_reorganize_all
    PROPERTIES
        TIMEOUT 300  # 5 minute timeout
        LABELS "unit;reorganize;cte"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_TEST_DATA_DIR=${CMAKE_BINARY_DIR}"
)

if(WRP_CORE_ENABLE_CUDA OR WRP_CORE_ENABLE_ROCM)
# GPU coroutine subtask tests (gpu2gpu dispatch + coroutine suspend/resume)
add_test(NAME cte_gpu_coroutine
    COMMAND test_cte_gpu_coroutine)

# GPU core tests run together in one process so Put and Get share the same
# initialized fixture and the stored blob persists between test cases.
add_test(NAME cte_gpu_core_all
    COMMAND test_gpu_core)

# GPU create tests - AsyncCreate from GPU kernel
add_test(NAME cte_gpu_create
    COMMAND test_gpu_create)

set_tests_properties(
    cte_gpu_coroutine
    PROPERTIES
        TIMEOUT 120
        LABELS "gpu;cte;coroutine;msan_skip"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
)

set_tests_properties(
    cte_gpu_core_all
    PROPERTIES
        TIMEOUT 120
        LABELS "gpu;cte;core;msan_skip"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
)

set_tests_properties(
    cte_gpu_create
    PROPERTIES
        TIMEOUT 120
        LABELS "gpu;cte;create;msan_skip"
        ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
)
endif()  # WRP_CORE_ENABLE_CUDA OR WRP_CORE_ENABLE_ROCM

# Mark all CTE tests as msan_skip (they use ZMQ via chimaera runtime which is uninstrumented)
set_tests_properties(
    cte_core_pool_creation
    cte_core_target_registration
    cte_core_blob_put
    cte_core_blob_get
    cte_core_integration
    cte_core_client_creation
    cte_core_params
    cte_core_target_config
    cte_core_tag_info
    cte_core_blob_info
    cte_core_tasks
    cte_core_helpers
    cte_core_workflow
    cte_core_performance
    cte_config_tests
    cte_dpe_tests
    cte_functional_all
    cte_query_tag_exact
    cte_query_tag_wildcard
    cte_query_tag_alternation
    cte_query_tag_matchall
    cte_query_tag_nomatch
    cte_query_blob_exact
    cte_query_blob_wildcard
    cte_query_blob_multitag
    cte_query_blob_matchall
    cte_query_blob_no_blob
    cte_query_blob_no_tag
    cte_query_local_poolquery
    cte_tag_construction
    cte_tag_putblob
    cte_tag_getblob
    cte_tag_errors
    cte_tag_metadata
    cte_tag_reorganize
    cte_tag_async
    cte_tag_large
    cte_tag_edge
    cte_client_config_all
    cte_runtime_coverage_all
    cte_tiered_storage_all
    cte_reorganize_all
    PROPERTIES LABELS "msan_skip"
)
if(WRP_CORE_ENABLE_CUDA OR WRP_CORE_ENABLE_ROCM)
set_tests_properties(
    cte_gpu_coroutine
    cte_gpu_core_all
    cte_gpu_create
    PROPERTIES LABELS "msan_skip"
)
endif()

# Tests that start a full Chimaera runtime share /tmp/chimaera_iowarp SHM
# segments and TCP/IPC ports, so they must NOT run in parallel.
set_tests_properties(
    cte_query_tag_exact
    cte_query_tag_wildcard
    cte_query_tag_alternation
    cte_query_tag_matchall
    cte_query_tag_nomatch
    cte_query_blob_exact
    cte_query_blob_wildcard
    cte_query_blob_multitag
    cte_query_blob_matchall
    cte_query_blob_no_blob
    cte_query_blob_no_tag
    cte_query_local_poolquery
    cte_tag_construction
    cte_tag_putblob
    cte_tag_getblob
    cte_tag_errors
    cte_tag_metadata
    cte_tag_reorganize
    cte_tag_async
    cte_tag_large
    cte_tag_edge
    cte_functional_all
    cte_client_config_all
    cte_runtime_coverage_all
    cte_tiered_storage_all
    cte_reorganize_all
    PROPERTIES RESOURCE_LOCK "chimaera_runtime"
)
if(WRP_CORE_ENABLE_CUDA OR WRP_CORE_ENABLE_ROCM)
set_tests_properties(
    cte_gpu_coroutine
    cte_gpu_core_all
    cte_gpu_create
    PROPERTIES RESOURCE_LOCK "chimaera_runtime"
)
endif()

# All cte_functional_* tests run test_core_functionality (9 test cases, each
# initializing and finalizing the full chimaera runtime).  After multiple
# runtime init/finalize cycles, ZMQ background threads fire callbacks on
# already-freed runtime memory, producing non-deterministic heap-use-after-free
# defects under ASan.  The same uninstrumented-runtime category caused all CTE
# tests to be msan_skip'd.  Skip the whole functional group under ASan.
set_tests_properties(
    cte_functional_all
    PROPERTIES LABELS "msan_skip;asan_skip"
)

# ------------------------------------------------------------------------------
# ------------------------------------------------------------------------------
# Install Targets
# ------------------------------------------------------------------------------
set(_CTE_TEST_TARGETS cte_core_unit_tests test_core_functionality test_query test_cte_config_dpe test_tag_operations test_core_client_config test_core_runtime_coverage test_tiered_storage_stress test_reorganize_blob)
# (Legacy test_cte_gpu_coroutine / test_gpu_core / test_gpu_create were
# removed along with the GPU runtime — see deletions earlier in this
# file. The new CUDA test target test_cuda_cte_kernel_putget is added
# below with its own install rule via add_cuda_executable.)
install(TARGETS ${_CTE_TEST_TARGETS}
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

# ------------------------------------------------------------------------------
# CTE PutBlob+GetBlob device-memory round-trip (CUDA / ROCm).
# Validates that the producer-only gpu2cpu path correctly handles tasks
# AND blob payload data that live entirely in kDeviceMem (no pinned
# host fallback).
# ------------------------------------------------------------------------------
if(WRP_CORE_ENABLE_CUDA)
  add_cuda_executable(test_cte_devmem_putget FALSE
    gpu/test_cte_devmem_putget.cc
    INCLUDE_DIRS
      ${CMAKE_SOURCE_DIR}/context-runtime/include
      ${CMAKE_SOURCE_DIR}/context-runtime/test
      ${CMAKE_SOURCE_DIR}/context-runtime/modules/admin/include
      ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include
    LINK_LIBS
      chimaera_cxx
      chimaera_cxx_gpu
      chimaera_admin_client
      chimaera_bdev_client
      wrp_cte_core_client
      wrp_cte_core_runtime
      hshm::cuda_cxx
      ${CMAKE_THREAD_LIBS_INIT}
  )
  set_target_properties(test_cte_devmem_putget PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME cte_devmem_putget_cuda COMMAND test_cte_devmem_putget
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  set_tests_properties(cte_devmem_putget_cuda PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_REPO_PATH=${CMAKE_BINARY_DIR}/bin"
    TIMEOUT 120
    LABELS "gpu;cuda;cte;devmem;putblob"
  )
endif()

if(WRP_CORE_ENABLE_ROCM)
  add_rocm_gpu_executable(test_cte_devmem_putget FALSE
    gpu/test_cte_devmem_putget.cc
  )
  target_compile_options(test_cte_devmem_putget PRIVATE
    "$<$<COMPILE_LANGUAGE:CUDA>:SHELL:--generate-code=arch=compute_89,code=sm_89>"
  )
  set_target_properties(test_cte_devmem_putget PROPERTIES
    CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  target_include_directories(test_cte_devmem_putget PUBLIC
    ${CMAKE_SOURCE_DIR}/context-runtime/include
    ${CMAKE_SOURCE_DIR}/context-runtime/test
    ${CMAKE_SOURCE_DIR}/context-runtime/modules/admin/include
    ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include
  )
  target_link_libraries(test_cte_devmem_putget PUBLIC
    chimaera_cxx
    chimaera_cxx_gpu
    chimaera_admin_client
    chimaera_bdev_client
    wrp_cte_core_client
    wrp_cte_core_runtime
    hshm::rocm_cxx
    ${CMAKE_THREAD_LIBS_INIT}
  )
  add_test(NAME cte_devmem_putget_rocm COMMAND test_cte_devmem_putget
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  set_tests_properties(cte_devmem_putget_rocm PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH};CHI_REPO_PATH=${CMAKE_BINARY_DIR}/bin"
    TIMEOUT 120
    LABELS "gpu;rocm;cte;devmem;putblob"
  )
endif()

# ------------------------------------------------------------------------------
# Note: Compression tests have been moved to compressor chimod tests
# See context-transfer-engine/compressor/test/ for compression functionality tests
# ------------------------------------------------------------------------------
# Add External Integration Tests
# ------------------------------------------------------------------------------
# External test demonstrates proper external linking patterns
# Temporarily commented out until ChiMod is installed and can be found as a package
# add_subdirectory(external)

# ------------------------------------------------------------------------------
# Add Adapter Unit Tests
# ------------------------------------------------------------------------------
add_subdirectory(adapters)