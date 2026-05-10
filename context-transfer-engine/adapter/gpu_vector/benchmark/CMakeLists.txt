#-----------------------------------------------------------------------------
# gpu_vector performance benchmark.
#
# Drives a Vector<chi::u32> through write/flush/read and prints
# bandwidth + latency. Knobs: --blocks, --pages-per-block, --page-size,
# --ratio, --total-bytes, --cache-period-ms, --iters, --no-read,
# --bdev-capacity-mib, --gpu-id.
#-----------------------------------------------------------------------------

if(WRP_CORE_ENABLE_CUDA)
    # Baseline cudaMemcpy bench (pure CUDA runtime; no chimaera deps).
    add_cuda_executable(wrp_memcpy_d2h_bench FALSE
        wrp_memcpy_d2h_bench.cc
        LINK_LIBS
            hshm::cuda_cxx
            ${CMAKE_THREAD_LIBS_INIT}
    )
    set_target_properties(wrp_memcpy_d2h_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    install(TARGETS wrp_memcpy_d2h_bench RUNTIME DESTINATION bin)

    add_cuda_executable(wrp_cte_gpu_vector_bench FALSE
        wrp_cte_gpu_vector_bench.cc
        INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/context-runtime/include
            ${CMAKE_SOURCE_DIR}/context-runtime/modules/admin/include
            ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include
            ${CMAKE_SOURCE_DIR}/context-transfer-engine/adapter/gpu_vector/include
        LINK_LIBS
            chimaera_cxx
            chimaera_cxx_gpu
            chimaera_admin_client
            chimaera_bdev_client
            wrp_cte_core_client
            wrp_cte_core_runtime
            wrp_cte::gpu_vector
            hshm::cuda_cxx
            ${CMAKE_THREAD_LIBS_INIT}
    )
    set_target_properties(wrp_cte_gpu_vector_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    install(TARGETS wrp_cte_gpu_vector_bench RUNTIME DESTINATION bin)
endif()

if(WRP_CORE_ENABLE_ROCM)
    add_rocm_gpu_executable(wrp_memcpy_d2h_bench FALSE
        wrp_memcpy_d2h_bench.cc
    )
    target_compile_options(wrp_memcpy_d2h_bench PRIVATE
        "$<$<COMPILE_LANGUAGE:CUDA>:SHELL:--generate-code=arch=compute_89,code=sm_89>"
    )
    set_target_properties(wrp_memcpy_d2h_bench PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    target_link_libraries(wrp_memcpy_d2h_bench PUBLIC
        hshm::rocm_cxx
        ${CMAKE_THREAD_LIBS_INIT}
    )
    install(TARGETS wrp_memcpy_d2h_bench RUNTIME DESTINATION bin)

    add_rocm_gpu_executable(wrp_cte_gpu_vector_bench FALSE
        wrp_cte_gpu_vector_bench.cc
    )
    target_compile_options(wrp_cte_gpu_vector_bench PRIVATE
        "$<$<COMPILE_LANGUAGE:CUDA>:SHELL:--generate-code=arch=compute_89,code=sm_89>"
    )
    set_target_properties(wrp_cte_gpu_vector_bench PROPERTIES
        CUDA_ARCHITECTURES "${CMAKE_CUDA_ARCHITECTURES}"
        CUDA_STANDARD 17
        CUDA_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    target_include_directories(wrp_cte_gpu_vector_bench PUBLIC
        ${CMAKE_SOURCE_DIR}/context-runtime/include
        ${CMAKE_SOURCE_DIR}/context-runtime/modules/admin/include
        ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include
        ${CMAKE_SOURCE_DIR}/context-transfer-engine/adapter/gpu_vector/include
    )
    target_link_libraries(wrp_cte_gpu_vector_bench PUBLIC
        chimaera_cxx
        chimaera_cxx_gpu
        chimaera_admin_client
        chimaera_bdev_client
        wrp_cte_core_client
        wrp_cte_core_runtime
        wrp_cte::gpu_vector
        hshm::rocm_cxx
        ${CMAKE_THREAD_LIBS_INIT}
    )
    install(TARGETS wrp_cte_gpu_vector_bench RUNTIME DESTINATION bin)
endif()
