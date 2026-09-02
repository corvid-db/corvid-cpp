# corvid-cpp — RAII C++20 bindings over the corvid database's C ABI.
#
# Offline-first by design (docs/PLAN.md): NO FetchContent, no network at
# configure/build time. The build links the RELEASE artifacts that
# fetch.sh / fetch.ps1 downloaded, sha256-verified, and extracted into
# deps/ — the cdylib, corvid.h, and the golden fixtures that ship inside
# the pinned tag's archive. Consuming THIS library from another project:
# FetchContent/add_subdirectory this repo (after running its fetch.sh) or
# find_package(corvid) against an installed package — see README.md.

# Floor 3.28 = Ubuntu 24.04 LTS system CMake (the oldest toolchain any
# supported platform or CI runner ships); per the bindings toolchain
# policy (engine scripts/bindings/README.md) there is no compat base to
# protect, so the floor stays modern.
cmake_minimum_required(VERSION 3.28)
project(corvid-cpp VERSION 0.1.0 LANGUAGES C CXX)

# ---- locate the fetched engine artifacts (deps/) ------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/version.txt")
    message(FATAL_ERROR
        "deps/version.txt is missing: the engine artifacts have not been "
        "fetched. Run ./fetch.sh (macOS/Linux) or ./fetch.ps1 (Windows) "
        "first — the build consumes deps/ only, by design.")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/deps/version.txt" CORVID_VERSION)
string(STRIP "${CORVID_VERSION}" CORVID_VERSION)

# The stamp carries the version; the extracted dir adds the host triple.
file(GLOB CORVID_ROOT_LIST
     "${CMAKE_CURRENT_SOURCE_DIR}/deps/corvid-ffi-${CORVID_VERSION}-*")
list(LENGTH CORVID_ROOT_LIST CORVID_ROOT_COUNT)
if(NOT CORVID_ROOT_COUNT EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one deps/corvid-ffi-${CORVID_VERSION}-<triple> "
        "directory, found ${CORVID_ROOT_COUNT}: re-run fetch.sh / fetch.ps1 "
        "(it discards stale versions and keeps exactly one)")
endif()
list(GET CORVID_ROOT_LIST 0 CORVID_ROOT)
message(STATUS "corvid-cpp: engine artifacts ${CORVID_VERSION} at ${CORVID_ROOT}")

if(WIN32)
    set(CORVID_LIB "${CORVID_ROOT}/corvid.dll")
    set(CORVID_IMPLIB "${CORVID_ROOT}/corvid.dll.lib")
elseif(APPLE)
    set(CORVID_LIB "${CORVID_ROOT}/libcorvid.dylib")
else()
    set(CORVID_LIB "${CORVID_ROOT}/libcorvid.so")
endif()

foreach(artifact "${CORVID_ROOT}/corvid.h" "${CORVID_LIB}")
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR "engine artifact missing: ${artifact}")
    endif()
endforeach()
if(WIN32 AND NOT EXISTS "${CORVID_IMPLIB}")
    message(FATAL_ERROR "engine import library missing: ${CORVID_IMPLIB}")
endif()

# The downloaded cdylib as an imported target: MSVC links the shipped
# import library (corvid.dll.lib); everyone else links the cdylib itself.
add_library(corvid_engine SHARED IMPORTED GLOBAL)
set_target_properties(corvid_engine PROPERTIES
    IMPORTED_LOCATION "${CORVID_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CORVID_ROOT}") # corvid.h at the root
if(WIN32)
    set_target_properties(corvid_engine PROPERTIES IMPORTED_IMPLIB "${CORVID_IMPLIB}")
endif()

# ---- the library ---------------------------------------------------------
# Header-first RAII over the C ABI: the public header never names an ABI
# type (scripts/idiom-gate.sh enforces it); this one TU is the whole impl.
add_library(corvidpp STATIC src/corvid.cpp)
add_library(corvid::corvid ALIAS corvidpp)
target_compile_features(corvidpp PUBLIC cxx_std_20)
set_target_properties(corvidpp PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO)
target_include_directories(corvidpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
# The engine's corvid.h lives with the downloaded artifacts; consumers of
# the installed package get it installed alongside our headers.
target_include_directories(corvidpp PRIVATE "${CORVID_ROOT}")
target_link_libraries(corvidpp PUBLIC corvid_engine)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
elseif(MSVC)
    # /utf-8: the examples' CJK strings are UTF-8 in the source and must
    # reach the engine as UTF-8 bytes (MSVC otherwise transcodes string
    # literals to the local codepage).
    add_compile_options(/W4 /utf-8)
endif()

# One predictable home for the executables (multi-config generators
# still append their config dir: build/bin/Release on MSVC).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")

# ---- consumer plumbing ----------------------------------------------------
# Wire one executable against the library: link, C++20, an rpath to
# deps/ for the build tree (macOS @rpath install name; Linux), an install
# rpath for system installs, and — on Windows — the DLL copied next to
# the binary so the loader always finds it.
function(corvidpp_consumer target)
    target_link_libraries(${target} PRIVATE corvid::corvid)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        BUILD_RPATH "${CORVID_ROOT}"
        INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CORVID_LIB}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "copying the engine dll for ${target}")
    endif()
endfunction()

# ---- the examples tour (examples/<name>.cpp) -------------------------------
# One runnable program per concept — quickstart, hybrid (RRF+MMR),
# vector-index families, text search (incl. CJK + the phrase API), graph
# + delete cascade, geo — each also registered as a ctest so CI executes
# the whole tour on every platform (and under the sanitizer job: every
# RAII wrapper frees its handle on its creation path).
set(CORVID_EXAMPLES quickstart hybrid vector_index text_search graph geo)
foreach(example IN LISTS CORVID_EXAMPLES)
    add_executable(example_${example} examples/${example}.cpp)
    corvidpp_consumer(example_${example})
endforeach()

# ---- the golden-suite port (test/golden.cpp) --------------------------------
# The core deliverable: the C++ port of the engine's harness (its
# crates/corvid-ffi/c/smoke.c at the pinned tag), driving the DOWNLOADED
# cdylib over the release's golden/ fixtures. One ctest per fixture file
# (each with its own scratch workdir for file-db scenarios). The harness
# drives the C ABI directly — it is the artifacts' contract check, kept
# independent of the RAII layer by design.
enable_testing()

add_executable(golden test/golden.cpp)
corvidpp_consumer(golden)
if(UNIX AND NOT APPLE)
    target_link_libraries(golden PRIVATE m) # libm for the harness's math
endif()

# ---- the frozen error-code table (test/errcodes.cpp) -----------------------
# Compile-time proof that every engine Error variant maps to its documented
# frozen code in the shipped corvid.h AND that this library's ErrorCode
# mirrors the same table.
add_executable(errcodes test/errcodes.cpp)
corvidpp_consumer(errcodes)

# ---- the RAII-layer unit tests (test/raii.cpp) ------------------------------
# Exercises the public C++ API this library exists to provide: literals
# and initializer-list construction, accessors, map_keys, the fluent
# query builder, phrase search, exceptions with code(), move semantics.
add_executable(raii_tests test/raii.cpp)
corvidpp_consumer(raii_tests)

file(GLOB CORVID_FIXTURES "${CORVID_ROOT}/golden/*.txt")
list(SORT CORVID_FIXTURES)
if(NOT CORVID_FIXTURES)
    message(FATAL_ERROR "no golden fixtures under ${CORVID_ROOT}/golden")
endif()
foreach(fixture ${CORVID_FIXTURES})
    get_filename_component(stem "${fixture}" NAME_WE)
    set(workdir "${CMAKE_CURRENT_BINARY_DIR}/scratch/${stem}")
    # Fresh workdir per run via the driver — the port of the engine
    # driver's per-run tempdir (test/run_fixture.cmake).
    add_test(NAME golden_${stem}
        COMMAND "${CMAKE_COMMAND}" -P
                "${CMAKE_CURRENT_SOURCE_DIR}/test/run_fixture.cmake"
                $<TARGET_FILE:golden> "${workdir}" "${fixture}")
endforeach()
message(STATUS "corvid-cpp: golden suite = ${CORVID_FIXTURES}")

add_test(NAME errcodes COMMAND errcodes)
add_test(NAME raii_tests COMMAND raii_tests)
foreach(example IN LISTS CORVID_EXAMPLES)
    add_test(NAME example_${example} COMMAND example_${example})
endforeach()

# ---- install + find_package(corvid) ----------------------------------------
# The config package is hand-written (not install(EXPORT)): corvidpp's
# dependency on the DOWNLOADED engine cdylib is an imported target at
# build time, which the export machinery cannot carry; installed
# consumers instead get two imported targets defined by corvidConfig
# .cmake — corvid::engine (the installed engine cdylib + its corvid.h)
# and corvid::corvid (the installed static library + headers, linking
# the engine).
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

configure_package_config_file(cmake/corvidConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/corvidConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/corvid")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/corvidConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)

install(TARGETS corvidpp
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CORVID_ROOT}/corvid.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/corvid/abi")
if(WIN32)
    install(FILES "${CORVID_LIB}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${CORVID_IMPLIB}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
else()
    install(FILES "${CORVID_LIB}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/corvidConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/corvidConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/corvid")
