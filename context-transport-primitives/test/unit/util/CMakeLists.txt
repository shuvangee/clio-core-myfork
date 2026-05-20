project(clio_ctp)

#------------------------------------------------------------------------------
# Build Tests
#------------------------------------------------------------------------------
add_executable(test_config_parse_exec
        ${TEST_MAIN}/main.cc
        test_init.cc
        test_config_parse.cc)
add_dependencies(test_config_parse_exec clio_ctp_host)
target_link_libraries(test_config_parse_exec
        clio_ctp_host
        Catch2::Catch2)

add_executable(test_numbers_exec
        ${TEST_MAIN}/main.cc
        test_init.cc
        test_numbers.cc)
add_dependencies(test_numbers_exec clio_ctp_host)
target_link_libraries(test_numbers_exec
        clio_ctp_host
        Catch2::Catch2)

#------------------------------------------------------------------------------
# Test Cases
#------------------------------------------------------------------------------
add_test(NAME ctp_config_parse COMMAND
        ${CMAKE_BINARY_DIR}/bin/test_config_parse_exec)

add_test(NAME ctp_numbers COMMAND
        ${CMAKE_BINARY_DIR}/bin/test_numbers_exec)

# Skip under MSan: uses precompiled Catch2 (FatalConditionHandler/sigaction
# false positives) and yaml-cpp (uninstrumented) for config parsing.
# ctp_numbers uses the same Catch2 harness, producing identical sigaction
# false positives even though the tested code is pure arithmetic.
set_tests_properties(
  ctp_config_parse
  ctp_numbers
  PROPERTIES LABELS "msan_skip"
)

#------------------------------------------------------------------------------
# Install Targets
#------------------------------------------------------------------------------
install(TARGETS
        test_config_parse_exec
        test_numbers_exec
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)
