cmake_minimum_required(VERSION 3.10)

# Create runtime library using modern ChiMod build functions
add_chimod_runtime(
  SOURCES
    src/core_runtime.cc
    src/core_config.cc
    src/core_dpe.cc
    src/autogen/core_lib_exec.cc
    # GPU runtime concept removed — kernels submit tasks to the CPU runtime
    # via gpu2cpu_queue and the CPU executes them through the standard
    # chi::Container dispatch path.
)

# Surface the GPU backend flags into the runtime library so the
# GpuMetadataCache helpers (which reach hshm::GpuApi::MallocManaged etc.)
# see the matching #if HSHM_ENABLE_{CUDA,ROCM,SYCL} branches. Without
# these the GPU code paths in core_runtime.cc are no-ops.
if(WRP_CORE_ENABLE_SYCL)
  target_compile_definitions(wrp_cte_core_runtime PRIVATE HSHM_ENABLE_SYCL=1)
  if(EXISTS "/opt/intel/dpcpp/include/sycl/sycl.hpp")
    target_include_directories(wrp_cte_core_runtime PRIVATE
      /opt/intel/dpcpp/include
      /opt/intel/dpcpp/include/sycl)
  elseif(EXISTS "/opt/adaptivecpp/include/AdaptiveCpp/sycl/sycl.hpp")
    target_include_directories(wrp_cte_core_runtime PRIVATE
      /opt/adaptivecpp/include/AdaptiveCpp)
  endif()
endif()
if(WRP_CORE_ENABLE_CUDA)
  target_compile_definitions(wrp_cte_core_runtime PRIVATE HSHM_ENABLE_CUDA=1)
endif()
if(WRP_CORE_ENABLE_ROCM)
  target_compile_definitions(wrp_cte_core_runtime PRIVATE HSHM_ENABLE_ROCM=1)
endif()

# Create client library using modern ChiMod build functions
add_chimod_client(
  SOURCES
    src/core_client.cc
    src/content_transfer_engine.cc
    src/tag.cc
)

# Note: Cross-namespace dependencies (chimaera admin and bdev) are automatically
# linked by add_chimod_* functions in ChimaeraCommon.cmake for non-chimaera namespaces
