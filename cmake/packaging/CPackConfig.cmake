#------------------------------------------------------------------------------
# CPack Configuration
# Handles DEB, RPM, and TGZ package generation with flexible control
#------------------------------------------------------------------------------

# Common CPack metadata
set(CPACK_PACKAGE_NAME "iowarp-core")
set(CPACK_PACKAGE_VENDOR "IOWarp Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "IOWarp Core: High-performance distributed I/O and task execution runtime")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "grc@illinoistech.edu")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# Only package the default (Unspecified) component — excludes pip_package files
# such as iowarp_core.pth which are wheel-only and must not land in system packages.
# CPACK_*_COMPONENT_INSTALL enables component-aware mode so CPACK_COMPONENTS_ALL
# is actually respected; without it CPack includes all components unconditionally.
set(CPACK_COMPONENTS_ALL Unspecified)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
# Keep a single combined package (don't split per-component)
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)

# Determine which generators to enable based on options
set(CPACK_GENERATORS_ENABLED OFF)

# DEB Package Configuration
if(CLIO_CORE_ENABLE_DEB_PACKAGE OR CLIO_CORE_ENABLE_CPACK)
    list(APPEND CPACK_GENERATOR "DEB")
    set(CPACK_GENERATORS_ENABLED ON)

    # DEB-specific settings
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "IOWarp Team <grc@illinoistech.edu>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Architecture: derive from the build host via `dpkg --print-architecture`
    # instead of hardcoding amd64. The previous hardcode shipped an "amd64"-
    # labeled .deb from the arm64 builder, and apt on arm64 then tried to
    # install it as a foreign arch and failed pulling :amd64 dependencies.
    # CPack falls back to dpkg auto-detection when CPACK_DEBIAN_PACKAGE_ARCHITECTURE
    # is unset.
    find_program(DPKG_CMD dpkg)
    if(DPKG_CMD)
        execute_process(COMMAND ${DPKG_CMD} --print-architecture
            OUTPUT_VARIABLE CPACK_DEBIAN_PACKAGE_ARCHITECTURE
            OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    # Runtime dependencies: external shared libraries our binaries NEED.
    # Be exhaustive here — by symmetry with the RPM side, where we turn
    # AUTOREQ off to silence false-positives on our internal libs, we
    # also lose auto-detection of true external deps. Match the set of
    # third-party .so files our binaries link against
    # (readelf -d clio_run | grep NEEDED, plus the same for the bench
    # binaries).
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libzmq5, libyaml-cpp0.8, libmsgpack-c2, libsodium23")

    message(STATUS "CPack: DEB generator enabled (arch=${CPACK_DEBIAN_PACKAGE_ARCHITECTURE})")
endif()

# RPM Package Configuration
if(CLIO_CORE_ENABLE_RPM_PACKAGE OR CLIO_CORE_ENABLE_CPACK)
    list(APPEND CPACK_GENERATOR "RPM")
    set(CPACK_GENERATORS_ENABLED ON)

    # RPM-specific settings
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "System/Libraries")
    # External shared-library deps. AUTOREQ is off below (otherwise rpm
    # generates spurious Requires on our OWN internal libs and fails to
    # self-resolve), so list every external lib we link against. Match
    # the NEEDED entries from `readelf -d clio_run` etc.
    set(CPACK_RPM_PACKAGE_REQUIRES "zeromq, yaml-cpp, msgpack-c, libsodium")
    # Disable auto-generated Requires on internal libraries. With AUTOREQ
    # default-on, rpmbuild scans every installed .so and adds a
    # Requires: lib<x>.so()(64bit) for each one — including our OWN
    # libclio_admin_client.so / libchimaera_MOD_NAME_*.so / etc. which
    # ARE in the same RPM. The matching Provides: side isn't generated
    # at the same path (sym-version mismatch under the cpack flow),
    # so dnf refuses to install with "nothing provides libclio_*". Turn
    # both directions off and rely on CPACK_RPM_PACKAGE_REQUIRES above
    # for the external deps that actually need declaring (zeromq,
    # yaml-cpp). Internal .so resolution happens at runtime via
    # rpath (`$ORIGIN/../lib`).
    set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
    set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
    # Disable Fedora's brp-check-rpaths spec-file post-policy. CMake bakes
    # an $ORIGIN/../lib rpath into clio_run / clio_cae / clio_cte_bench so
    # they can find their sibling libclio_*.so files at /usr/lib/. Fedora's
    # rpmbuild macros run check-rpaths-worker as part of the install-time
    # %install scriptlet, which sees the $ORIGIN-relative rpath and strips
    # it on the grounds that "binaries in /usr/bin shouldn't need rpath."
    # Net effect: `clio_run --help` exits 127 with
    #   error while loading shared libraries: libclio_admin_client.so:
    #   cannot open shared object file
    # because ld.so falls back to LD_LIBRARY_PATH + ld.so.cache and finds
    # no entry for our libs. Disabling the brp check preserves the same
    # install layout the .deb already uses (which Debian's policy doesn't
    # touch), so both packages now resolve their internal libs identically.
    set(CPACK_RPM_SPEC_MORE_DEFINE "%define __brp_check_rpaths %{nil}")

    message(STATUS "CPack: RPM generator enabled")
endif()

# Legacy TGZ support (from CLIO_CORE_ENABLE_CPACK)
if(CLIO_CORE_ENABLE_CPACK)
    if(NOT "TGZ" IN_LIST CPACK_GENERATOR)
        list(APPEND CPACK_GENERATOR "TGZ")
    endif()
    set(CPACK_GENERATORS_ENABLED ON)

    message(STATUS "CPack: TGZ generator enabled (legacy CLIO_CORE_ENABLE_CPACK)")
endif()

# Only include CPack if at least one generator is enabled
if(CPACK_GENERATORS_ENABLED)
    include(CPack)
    message(STATUS "CPack configuration: ${CPACK_GENERATOR}")
else()
    message(STATUS "CPack disabled (no package generators enabled)")
endif()
