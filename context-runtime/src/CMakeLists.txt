# src/CMakeLists.txt
# Main Chimaera library source code

cmake_minimum_required(VERSION 3.10)

# Get all source files, excluding GPU-specific files
file(GLOB_RECURSE CHIMAERA_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

# Collect GPU sources from modules (not picked up by the src/ glob above)
# Only match src/ subdirectories to avoid picking up test files.
file(GLOB_RECURSE CHIMAERA_MODULE_GPU_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/*/src/*_gpu.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/*/src/*_sycl.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/*/src/*_hip.cc
)

# Separate GPU sources (_gpu.cc / _sycl.cc / _hip.cc files) from CPU
# sources. _gpu.cc and _hip.cc are compiled by NVCC/HIPCC under
# WRP_CORE_ENABLE_CUDA/ROCM, and skipped under SYCL via their own
# `#if HSHM_ENABLE_CUDA || HSHM_ENABLE_ROCM` guard. _sycl.cc is the
# SYCL twin: gated by `#if HSHM_ENABLE_SYCL` and compiled by
# add_sycl_library() when WRP_CORE_ENABLE_SYCL is on.
set(CHIMAERA_GPU_SOURCES "")
set(CHIMAERA_CPU_SOURCES "")
foreach(src ${CHIMAERA_SOURCES})
  if(src MATCHES "_(gpu|sycl|hip)\\.cc$")
    list(APPEND CHIMAERA_GPU_SOURCES ${src})
  else()
    list(APPEND CHIMAERA_CPU_SOURCES ${src})
  endif()
endforeach()

# Module GPU sources are always GPU-only
foreach(src ${CHIMAERA_MODULE_GPU_SOURCES})
  list(APPEND CHIMAERA_GPU_SOURCES ${src})
endforeach()

# Collect GPU sources from external modules (e.g., CTE core) that register
# GPU containers in the orchestrator but live outside context-runtime/modules/.
file(GLOB_RECURSE CHIMAERA_EXTERNAL_GPU_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/src/*_gpu.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/src/*_sycl.cc
)
foreach(src ${CHIMAERA_EXTERNAL_GPU_SOURCES})
  list(APPEND CHIMAERA_GPU_SOURCES ${src})
endforeach()

# Create main library with namespace prefix to avoid conflicts
set(CHIMAERA_LIB_NAME "${CHIMAERA_NAMESPACE}_cxx")
add_library(${CHIMAERA_LIB_NAME} SHARED ${CHIMAERA_CPU_SOURCES})

# Build GPU work orchestrator library if CUDA/ROCm/SYCL enabled and GPU sources exist
if(CHIMAERA_GPU_SOURCES)
  if(WRP_CORE_ENABLE_CUDA)
    add_cuda_library(${CHIMAERA_LIB_NAME}_gpu SHARED TRUE ${CHIMAERA_GPU_SOURCES}
      INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/MOD_NAME/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/bdev/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_include_directories(${CHIMAERA_LIB_NAME}_gpu PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/MOD_NAME/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/bdev/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    target_link_libraries(${CHIMAERA_LIB_NAME}_gpu PUBLIC hshm::cuda_cxx)
    # PRIVATE link: prevents HSHM_ENABLE_CUDA from propagating to g++ compilations
    # of chimaera_cxx sources (transport-primitives headers use CUDA builtins
    # guarded by HSHM_ENABLE_CUDA which are not available in g++)
    target_link_libraries(${CHIMAERA_LIB_NAME} PRIVATE ${CHIMAERA_LIB_NAME}_gpu)
  elseif(WRP_CORE_ENABLE_SYCL)
    add_sycl_library(${CHIMAERA_LIB_NAME}_gpu SHARED TRUE ${CHIMAERA_GPU_SOURCES}
      INCLUDE_DIRS
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/MOD_NAME/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../modules/bdev/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_include_directories(${CHIMAERA_LIB_NAME}_gpu PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/MOD_NAME/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/bdev/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    target_link_libraries(${CHIMAERA_LIB_NAME}_gpu PUBLIC hshm::cxx)
    target_link_libraries(${CHIMAERA_LIB_NAME} PRIVATE ${CHIMAERA_LIB_NAME}_gpu)
    # Add HSHM_ENABLE_SYCL=1 + sycl headers to the host library (NOT -fsycl,
    # which would force a device-pass parse of every host TU and trip
    # type-mismatches on host-only methods accessed via CHI_IPC). Without
    # -fsycl, host code uses sycl::malloc_host / sycl::queue / sycl::free
    # as plain runtime API calls. The chimaera_cxx_gpu library compiled by
    # add_sycl_library still gets full -fsycl + device-pass treatment.
    target_compile_definitions(${CHIMAERA_LIB_NAME} PRIVATE HSHM_ENABLE_SYCL=1)
    if(EXISTS "/opt/intel/dpcpp/include/sycl/sycl.hpp")
      target_include_directories(${CHIMAERA_LIB_NAME} PRIVATE
          /opt/intel/dpcpp/include
          /opt/intel/dpcpp/include/sycl)
    elseif(EXISTS "/opt/adaptivecpp/include/AdaptiveCpp/sycl/sycl.hpp")
      target_include_directories(${CHIMAERA_LIB_NAME} PRIVATE
          /opt/adaptivecpp/include/AdaptiveCpp)
    endif()
  elseif(WRP_CORE_ENABLE_ROCM)
    add_rocm_gpu_library(${CHIMAERA_LIB_NAME}_gpu SHARED FALSE ${CHIMAERA_GPU_SOURCES})
    target_include_directories(${CHIMAERA_LIB_NAME}_gpu PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/MOD_NAME/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/bdev/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transfer-engine/core/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    # Link hshm::rocm_cxx so HSHM_ENABLE_ROCM=1 propagates to chimaera_cxx
    # (PRIVATE link to ${CHIMAERA_LIB_NAME}_gpu carries those compile
    # definitions into chimaera_cxx's own TUs without exposing them to
    # external consumers — same model as the CUDA branch above).
    target_link_libraries(${CHIMAERA_LIB_NAME}_gpu PUBLIC hshm::rocm_cxx)
    target_link_libraries(${CHIMAERA_LIB_NAME} PRIVATE ${CHIMAERA_LIB_NAME}_gpu)
  endif()
endif()

# Set include directories as PUBLIC so they propagate to consumers
# Use CMAKE_CURRENT_SOURCE_DIR/.. to get runtime directory (this is runtime/src)
# Also include hermes_shm headers which are in context-transport-primitives/include
target_include_directories(${CHIMAERA_LIB_NAME} PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../modules/admin/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../context-transport-primitives/include>
  $<INSTALL_INTERFACE:include>
)

# When CUDA is enabled, g++ source files get HSHM_ENABLE_CUDA=1 transitively from
# the GPU library link. They need the CUDA toolkit include directory so that
# cuda_runtime_api.h (host-callable CUDA runtime functions) can be found.
if(WRP_CORE_ENABLE_CUDA AND CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES)
  find_package(CUDAToolkit QUIET)
  target_include_directories(${CHIMAERA_LIB_NAME} PUBLIC
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
  )
  # Link cudart for host-callable CUDA runtime functions used in ipc_manager.cc etc.
  target_link_libraries(${CHIMAERA_LIB_NAME} PUBLIC CUDA::cudart)
endif()

# ROCm twin: g++ source files get HSHM_ENABLE_ROCM=1 from the chimaera_cxx_gpu
# PRIVATE link, so they need the HIP host-API headers (hip/hip_runtime_api.h) to
# resolve hipMalloc / hipMemcpy / hipIpcMemHandle_t etc. Under HIP_PLATFORM=nvidia
# the HIP runtime forwards to cudart, so cudart needs to be linked too.
if(WRP_CORE_ENABLE_ROCM)
  target_include_directories(${CHIMAERA_LIB_NAME} PUBLIC ${ROCM_ROOT}/include)
  target_compile_definitions(${CHIMAERA_LIB_NAME} PUBLIC HSHM_ENABLE_ROCM=1)
  if(WRP_ROCM_HIP_PLATFORM STREQUAL "nvidia")
    target_compile_definitions(${CHIMAERA_LIB_NAME} PUBLIC
      __HIP_PLATFORM_NVIDIA__=1
      __HIP_PLATFORM_NVCC__=1)
    find_package(CUDAToolkit QUIET)
    if(TARGET CUDA::cudart)
      target_link_libraries(${CHIMAERA_LIB_NAME} PUBLIC CUDA::cudart)
    endif()
    if(CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES)
      target_include_directories(${CHIMAERA_LIB_NAME} PUBLIC
        ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
    endif()
  endif()
endif()

# Add runtime definition
# Removed CHIMAERA_RUNTIME compile definition - using runtime IsRuntime() checks instead

# Add CMAKE_INSTALL_PREFIX as a compile definition for ChiMod search paths
target_compile_definitions(${CHIMAERA_LIB_NAME} PRIVATE
  CHI_INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}"
)

# Link libraries
# Chimaera runtime links to hshm modular targets
# Clients only link to hshm::cxx
target_link_libraries(${CHIMAERA_LIB_NAME}
  PUBLIC
    hshm::cxx
    hshm::serialize  # For cereal
    Threads::Threads
    hshm::lightbeam  # For ZMQ transport
    # chimods_admin_client  # Admin client library for global task routing - TODO: Enable when admin chimod is built
)

# Set target properties
set_target_properties(${CHIMAERA_LIB_NAME} PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  PUBLIC_HEADER "${CHIMAERA_ROOT}/include/chimaera/chimaera.h"
  OUTPUT_NAME "${CHIMAERA_NAMESPACE}_cxx"
)

# Export target for parent scope
set_target_properties(${CHIMAERA_LIB_NAME} PROPERTIES EXPORT_NAME cxx)

# Precompiled headers for faster builds. Skipped under SYCL because the
# SYCL flag set (`-fsycl`) is incompatible with clang's PCH bundler:
# clang-offload-bundler refuses 'pch' as an input file type.
if(NOT WRP_CORE_ENABLE_SYCL)
  target_precompile_headers(${CHIMAERA_LIB_NAME} PRIVATE
      <string> <vector> <memory> <unordered_map>
      <functional> <algorithm> <cstdint> <cstring> <iostream>
      <sstream> <fstream> <chrono> <mutex> <thread>
  )
endif()

# Create namespace alias for external consumption
add_library(${CHIMAERA_NAMESPACE}::cxx ALIAS ${CHIMAERA_LIB_NAME})
