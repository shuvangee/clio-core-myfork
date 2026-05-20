# STDIO Adapter Unit Tests

include_directories(
    ${CLIO_CTE_ROOT}
    .
)

add_executable(clio_cte_stdio_unit_tests
    test_stdio_adapter.cc
)

target_link_libraries(clio_cte_stdio_unit_tests
    clio_cte_stdio
    clio_cte_posix       # for shared PosixFs::IsFdTracked used by stdio
    clio_cte_fs_base
    clio_cte_core_client
    ctp::cxx
    ctp::interceptor
    Catch2::Catch2WithMain
)

if(CLIO_CORE_ENABLE_MPI)
    target_link_libraries(clio_cte_stdio_unit_tests MPI::MPI_CXX)
endif()

install(
    TARGETS clio_cte_stdio_unit_tests
    RUNTIME DESTINATION bin
)
