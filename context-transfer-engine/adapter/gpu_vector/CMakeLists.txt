#-----------------------------------------------------------------------------
# GPU Vector adapter — header-only library.
#
# All kernel launches and device functions live in
# include/wrp_cte/gpu_vector/*.h and are instantiated by the user's
# CUDA / HIP TU. This INTERFACE library carries the include path and
# transitive link to the producer-only chimaera GPU stack
# (chimaera_cxx + chimaera_cxx_gpu + wrp_cte_core_client).
#-----------------------------------------------------------------------------

add_library(wrp_cte_gpu_vector INTERFACE)

target_include_directories(wrp_cte_gpu_vector INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(wrp_cte_gpu_vector INTERFACE
    chimaera_cxx
    chimaera_cxx_gpu
    chimaera_admin_client
    wrp_cte_core_client
)

add_library(wrp_cte::gpu_vector ALIAS wrp_cte_gpu_vector)

#-----------------------------------------------------------------------------
# Tests
#-----------------------------------------------------------------------------
if(WRP_CORE_ENABLE_TESTS)
    add_subdirectory(test)
endif()

#-----------------------------------------------------------------------------
# Benchmarks
#-----------------------------------------------------------------------------
add_subdirectory(benchmark)

#-----------------------------------------------------------------------------
# Install headers
#-----------------------------------------------------------------------------
install(DIRECTORY include/wrp_cte/gpu_vector
    DESTINATION include/wrp_cte
    COMPONENT headers
    FILES_MATCHING PATTERN "*.h"
)
