cmake_minimum_required(VERSION 3.10)

# Collect optional NIXL dependency
set(BDEV_OPTIONAL_LIBS "")
if(CLIO_CORE_ENABLE_NIXL)
  list(APPEND BDEV_OPTIONAL_LIBS ctp::nixl)
endif()

# Collect optional CUDA dependency for kHbm/kPinned bdev types
set(BDEV_GPU_LIBS "")
if(CLIO_CORE_ENABLE_CUDA)
  list(APPEND BDEV_GPU_LIBS ctp::cuda_cxx)
elseif(CLIO_CORE_ENABLE_ROCM)
  list(APPEND BDEV_GPU_LIBS ctp::rocm_cxx)
endif()

# Client library — ctp::aio provides async I/O headers via its INTERFACE includes
add_chimod_client(
  SOURCES
    src/bdev_client.cc
  LINK_LIBRARIES
    chimaera_admin_client
    ctp::aio
    ${BDEV_OPTIONAL_LIBS}
)

# Runtime library
add_chimod_runtime(
  SOURCES
    src/bdev_runtime.cc
    src/autogen/bdev_lib_exec.cc
  LINK_LIBRARIES
    chimaera_admin_runtime
    ctp::aio
    ${BDEV_OPTIONAL_LIBS}
    ${BDEV_GPU_LIBS}
)

# SYCL HBM bdev support: a separate task. Adding -fsycl to
# chimaera_bdev_runtime conflicts with the runtime's precompiled
# headers (clang-offload-bundler rejects .pch). When that's resolved
# (likely needs PCH disabled for SYCL builds), enable here:
#
#   if(CLIO_CORE_ENABLE_SYCL AND TARGET chimaera_bdev_runtime)
#     set_target_properties(chimaera_bdev_runtime PROPERTIES
#       DISABLE_PRECOMPILE_HEADERS ON)
#     wrp_core_apply_sycl_flags(chimaera_bdev_runtime)
#   endif()
#
# Until then, the kHbm bdev path under SYCL is compiled out by the
# `#if CTP_ENABLE_SYCL && !(CUDA||ROCM)` guard in src/bdev_runtime.cc.

# Unit tests
if(CLIO_CORE_ENABLE_TESTS)
  add_subdirectory(test)
endif()
