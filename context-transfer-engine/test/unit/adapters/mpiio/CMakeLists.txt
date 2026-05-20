# MPI-IO Adapter Unit Tests
#
# This test exercises the WRP CTE MPI-IO adapter via direct linking
# (no LD_PRELOAD). It only builds when MPI is enabled.

if(NOT WRP_CORE_ENABLE_MPI)
    return()
endif()

include_directories(
    ${WRP_CTE_ROOT}
    .
)

add_executable(clio_cte_mpiio_unit_tests
    test_mpiio_adapter.cc
)

target_link_libraries(clio_cte_mpiio_unit_tests
    wrp_cte_mpiio
    wrp_cte_fs_base
    wrp_cte_core_client
    MPI::MPI_CXX
    hshm::cxx
    hshm::interceptor
    Catch2::Catch2WithMain
)

install(
    TARGETS clio_cte_mpiio_unit_tests
    RUNTIME DESTINATION bin
)
