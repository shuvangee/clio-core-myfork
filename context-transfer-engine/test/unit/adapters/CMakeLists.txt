# Adapter Unit Tests

# Add subdirectories for each adapter based on enabled options
# Note: All adapters require CLIO_CORE_ENABLE_ELF=ON except ADIOS2
if(CLIO_CORE_ENABLE_ELF AND CLIO_CTE_ENABLE_POSIX_ADAPTER)
    add_subdirectory(posix)
endif()

if(CLIO_CORE_ENABLE_ELF AND CLIO_CTE_ENABLE_STDIO_ADAPTER)
    add_subdirectory(stdio)
endif()

if(CLIO_CORE_ENABLE_ELF AND CLIO_CTE_ENABLE_MPIIO_ADAPTER AND CLIO_CORE_ENABLE_MPI)
    add_subdirectory(mpiio)
endif()

# ADIOS2 adapter tests (doesn't require ELF interception)
# Only add if ADIOS2 adapter is enabled
if(CLIO_CTE_ENABLE_ADIOS2_ADAPTER)
    add_subdirectory(adios2)
endif()

# FUSE adapter tests (doesn't require ELF or libfuse3 — tests internal components)
if(CLIO_CTE_ENABLE_FUSE_ADAPTER)
    add_subdirectory(libfuse)
endif()