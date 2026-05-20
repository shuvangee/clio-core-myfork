project(clio_ctp_external_test)

# ------------------------------------------------------------------------------
# External libraries
# ------------------------------------------------------------------------------
# Use unified IOWarp Core package which includes ClioCtp
find_package(iowarp-core CONFIG REQUIRED)
message(STATUS "found IowarpCore at ${IowarpCore_DIR}")
message(STATUS "  ClioCtp (context-transport-primitives) included in IowarpCore")

# ------------------------------------------------------------------------------
# Build
# ------------------------------------------------------------------------------
add_executable(test test.cc)
target_link_libraries(test ctp::cxx)

# hshm_enable_cuda(17)
# add_cuda_executable(test_cuda FALSE test_cuda.cc)
# target_link_libraries(test_cuda ctp::cudacxx)