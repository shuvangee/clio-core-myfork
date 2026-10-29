cmake_minimum_required(VERSION 3.10)
project(clio_cae_code_indexer)

#------------------------------------------------------------------------------
# Code Indexer — summarize -> embed -> search over a source tree, in memory.
#
# Two static libraries, split so the pure pipeline does not drag in the CAE
# runtime:
#   clio_cae_code_indexer            scanner, chunker, vector/BM25 index,
#                                    HTTP embedding client, CodeIndexer
#                                    (libcurl + nlohmann_json only)
#   clio_cae_code_indexer_summarize  summarizer built on the CAE Summarizer's
#                                    GenerateSummary (links the runtime)
#
# Built only when CAE_SUMMARIZATION_ENABLED (libcurl + nlohmann_json found,
# detected in ../CMakeLists.txt): the embedding client needs the same pair.
#------------------------------------------------------------------------------

add_library(clio_cae_code_indexer STATIC
  src/embed_client.cc
  src/code_indexer.cc
  src/claude_client.cc
  src/claude_cli.cc
)
target_include_directories(clio_cae_code_indexer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(clio_cae_code_indexer PUBLIC CURL::libcurl)
# nlohmann_json is header-only and may be a FetchContent (non-imported) target;
# take its include path without a link dependency so the static libraries can
# join this module's install(EXPORT) set.
target_include_directories(clio_cae_code_indexer PRIVATE
  $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>
)

add_library(clio_cae_code_indexer_summarize STATIC
  src/ollama_summarizer.cc
)
target_link_libraries(clio_cae_code_indexer_summarize
  PUBLIC
    clio_cae_code_indexer
    clio_cae_summarizer_runtime
    clio_cae_summarizer_client
    clio_cte_core_client
    ctp::cxx
)

# The runtime plugin links these statics, so they must be in the module's
# export set (named after the project, as add_clio_module_* installs it).
install(TARGETS clio_cae_code_indexer clio_cae_code_indexer_summarize
  EXPORT clio_cae_code_indexer
  ARCHIVE DESTINATION lib
)

#------------------------------------------------------------------------------
# ChiMod: a pool owning one in-memory index, answering IndexPath / Query /
# Status. Autogen (methods header + lib_exec) comes from `clio_run repo
# refresh` (module listed in ../clio_repo.yaml).
#------------------------------------------------------------------------------
add_clio_module_client(
  SOURCES
    src/code_indexer_client.cc
    src/cte_ingest.cc
)
target_link_libraries(clio_cae_code_indexer_client PUBLIC clio_cte_filesystem_client)
target_include_directories(clio_cae_code_indexer_client PRIVATE
  ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include)

add_clio_module_runtime(
  SOURCES
    src/code_indexer_runtime.cc
    src/autogen/code_indexer_lib_exec.cc
  LINK_LIBRARIES
    clio_cae_code_indexer_summarize
    clio_cte_core_client
    clio_cte_filesystem_client
)
target_include_directories(clio_cae_code_indexer_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include)

#------------------------------------------------------------------------------
# clio_code_index CLI
#------------------------------------------------------------------------------
add_executable(clio_code_index util/clio_code_index.cc)
target_link_libraries(clio_code_index
  PRIVATE
    clio_cae_code_indexer_summarize
    clio_cae_code_indexer_client
    clio::run::cxx
    clio::run::admin_runtime
    clio::run::bdev_runtime
    ${CMAKE_THREAD_LIBS_INIT}
)
install(TARGETS clio_code_index DESTINATION bin)

# Client for a running pool (connects to the CLIO runtime as a client).
add_executable(clio_code_index_client util/clio_code_index_client.cc)
target_link_libraries(clio_code_index_client
  PRIVATE
    clio_cae_code_indexer_client
    clio::run::cxx
    ctp::cxx
    ${CMAKE_THREAD_LIBS_INIT}
)
install(TARGETS clio_code_index_client DESTINATION bin)

#------------------------------------------------------------------------------
# Unit tests (in-process HTTP stub, no inference server needed)
#------------------------------------------------------------------------------
if(BUILD_TESTING)
  set(CAE_UNIT_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../test/unit)

  add_executable(test_code_indexer
    test/test_code_indexer.cc
    ${CAE_UNIT_TEST_DIR}/summarizer_http_stub.cc
  )
  target_include_directories(test_code_indexer
    PRIVATE
      ${CAE_UNIT_TEST_DIR}
      ${CMAKE_SOURCE_DIR}/context-runtime/test
  )
  target_link_libraries(test_code_indexer
    PRIVATE
      clio_cae_code_indexer_summarize
      clio::run::admin_runtime
      clio::run::bdev_runtime
      ${CMAKE_THREAD_LIBS_INIT}
  )
  if(WIN32)
    target_link_libraries(test_code_indexer PRIVATE ws2_32)
  endif()

  # The pool, end to end through a running runtime (HTTP stub as the model).
  add_executable(test_code_indexer_pool
    test/test_code_indexer_pool.cc
    ${CAE_UNIT_TEST_DIR}/summarizer_http_stub.cc
  )
  target_include_directories(test_code_indexer_pool
    PRIVATE
      ${CAE_UNIT_TEST_DIR}
      ${CMAKE_SOURCE_DIR}/context-runtime/test
  )
  target_link_libraries(test_code_indexer_pool
    PRIVATE
      clio_cae_code_indexer_client
      clio_cae_code_indexer_runtime
      clio::run::cxx
      clio::run::admin_runtime
      clio::run::bdev_runtime
      ctp::cxx
      ${YAML_CPP_LIBS}
      ${CMAKE_THREAD_LIBS_INIT}
  )
  if(WIN32)
    target_link_libraries(test_code_indexer_pool PRIVATE ws2_32)
  endif()
  add_test(NAME code_indexer_pool COMMAND test_code_indexer_pool)
  set_tests_properties(code_indexer_pool PROPERTIES
    TIMEOUT 120
    LABELS "cae;code_indexer;integration;msan_skip"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
  )
  install(TARGETS test_code_indexer_pool DESTINATION bin)

  # Summary persistence through a CTE core (RAM tier) in the same runtime.
  add_executable(test_code_indexer_persist
    test/test_code_indexer_persist.cc
    ${CAE_UNIT_TEST_DIR}/summarizer_http_stub.cc
  )
  target_include_directories(test_code_indexer_persist
    PRIVATE
      ${CAE_UNIT_TEST_DIR}
      ${CMAKE_SOURCE_DIR}/context-runtime/test
      ${CMAKE_SOURCE_DIR}/context-transfer-engine/core/include
  )
  target_link_libraries(test_code_indexer_persist
    PRIVATE
      clio_cae_code_indexer_client
      clio_cae_code_indexer_runtime
      clio_cte_core_client
      clio_cte_core_runtime
      clio::run::cxx
      clio::run::admin_runtime
      clio::run::bdev_runtime
      ctp::cxx
      ${YAML_CPP_LIBS}
      ${CMAKE_THREAD_LIBS_INIT}
  )
  if(WIN32)
    target_link_libraries(test_code_indexer_persist PRIVATE ws2_32)
  endif()
  add_test(NAME code_indexer_persist COMMAND test_code_indexer_persist)
  set_tests_properties(code_indexer_persist PROPERTIES
    TIMEOUT 120
    LABELS "cae;code_indexer;integration;msan_skip"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
  )
  install(TARGETS test_code_indexer_persist DESTINATION bin)

  add_test(NAME code_indexer COMMAND test_code_indexer)
  set_tests_properties(code_indexer PROPERTIES
    TIMEOUT 120
    LABELS "coverage;cae;code_indexer;msan_skip"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
  )
  install(TARGETS test_code_indexer DESTINATION bin)
endif()
