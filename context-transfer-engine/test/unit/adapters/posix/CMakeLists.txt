# POSIX Adapter Unit Tests

include_directories(
    ${CLIO_CTE_ROOT}
    .
)

# Create the POSIX adapter unit test
add_executable(clio_cte_posix_unit_tests
    test_posix_adapter.cc
)

target_link_libraries(clio_cte_posix_unit_tests
    clio_cte_posix
    clio_cte_fs_base
    clio_cte_core_client
    ctp::cxx
    ctp::interceptor
    Catch2::Catch2WithMain
)

# Link MPI only if enabled
if(CLIO_CORE_ENABLE_MPI)
    target_link_libraries(clio_cte_posix_unit_tests MPI::MPI_CXX)
endif()

# Install the test executable
install(
    TARGETS clio_cte_posix_unit_tests
    RUNTIME DESTINATION bin
)