include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_cuda_library(clio_cte_nvidia_gds SHARED TRUE cufile_api.cc)
target_link_libraries(clio_cte_nvidia_gds PUBLIC ctp::cudacxx MPI::MPI_CXX)

install(TARGETS clio_cte_nvidia_gds
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        )