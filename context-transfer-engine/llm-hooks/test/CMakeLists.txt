cmake_minimum_required(VERSION 3.20)

# KV cache round-trip test
add_executable(test_iowarp_kvcache test_kvcache.cc)
target_link_libraries(test_iowarp_kvcache PRIVATE clio_llm_kvcache Catch2::Catch2WithMain)
add_test(NAME test_iowarp_kvcache COMMAND test_iowarp_kvcache)

# Weight offloading smoke test (CUDA only)
if(CLIO_CORE_ENABLE_CUDA)
    add_executable(test_iowarp_weights test_weights.cc)
    target_link_libraries(test_iowarp_weights PRIVATE clio_llm_weights Catch2::Catch2WithMain)
    add_test(NAME test_iowarp_weights COMMAND test_iowarp_weights)
endif()
