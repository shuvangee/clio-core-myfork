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
)
target_include_directories(clio_cae_code_indexer PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(clio_cae_code_indexer
  PUBLIC  CURL::libcurl
  PRIVATE nlohmann_json::nlohmann_json
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

#------------------------------------------------------------------------------
# clio_code_index CLI
#------------------------------------------------------------------------------
add_executable(clio_code_index util/clio_code_index.cc)
target_link_libraries(clio_code_index
  PRIVATE
    clio_cae_code_indexer_summarize
    clio::run::admin_runtime
    clio::run::bdev_runtime
    ${CMAKE_THREAD_LIBS_INIT}
)
install(TARGETS clio_code_index DESTINATION bin)

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

  add_test(NAME code_indexer COMMAND test_code_indexer)
  set_tests_properties(code_indexer PROPERTIES
    TIMEOUT 120
    LABELS "coverage;cae;code_indexer;msan_skip"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/bin:$ENV{LD_LIBRARY_PATH}"
  )
  install(TARGETS test_code_indexer DESTINATION bin)
endif()
