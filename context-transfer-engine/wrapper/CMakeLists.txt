# Python bindings are controlled by CLIO_CORE_ENABLE_PYTHON
if(CLIO_CORE_ENABLE_PYTHON)
    message(STATUS "CTE Python bindings enabled")
    add_subdirectory(python)
else()
    message(STATUS "CTE Python bindings disabled: CLIO_CORE_ENABLE_PYTHON is OFF")
endif()

# Rust bindings (built independently via cargo)
# Build with: cd rust && cargo build
# See rust/README for details
