# STDIO Adapter Unit Tests

include_directories(
    ${WRP_CTE_ROOT}
    .
)

add_executable(clio_cte_stdio_unit_tests
    test_stdio_adapter.cc
)

target_link_libraries(clio_cte_stdio_unit_tests
    wrp_cte_stdio
    wrp_cte_posix       # for shared PosixFs::IsFdTracked used by stdio
    wrp_cte_fs_base
    wrp_cte_core_client
    hshm::cxx
    hshm::interceptor
    Catch2::Catch2WithMain
)

if(WRP_CORE_ENABLE_MPI)
    target_link_libraries(clio_cte_stdio_unit_tests MPI::MPI_CXX)
endif()

install(
    TARGETS clio_cte_stdio_unit_tests
    RUNTIME DESTINATION bin
)
