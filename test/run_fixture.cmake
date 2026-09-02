# run_fixture.cmake — the ctest driver for one golden fixture.
#
# The engine's Rust driver runs the harness in a fresh tempdir per
# invocation (crates/corvid-ffi/src/smoke.rs); the file-db scenarios
# (FILEDB / REOPEN / DUMP / BACKUP …) must not see stale .redb/.dump/
# .backup files from a previous ctest. This script is the standalone
# port of that responsibility: wipe + recreate the workdir, run the
# harness, and fail loudly on a nonzero exit.
#
# Usage (ctest): cmake -P run_fixture.cmake <golden-binary> <workdir> <fixture>

if(NOT DEFINED CMAKE_ARGV5)
    message(FATAL_ERROR "usage: cmake -P run_fixture.cmake <golden> <workdir> <fixture>")
endif()
set(golden   "${CMAKE_ARGV3}")
set(workdir  "${CMAKE_ARGV4}")
set(fixture  "${CMAKE_ARGV5}")

file(REMOVE_RECURSE "${workdir}")
file(MAKE_DIRECTORY "${workdir}")
execute_process(COMMAND "${golden}" "${workdir}" "${fixture}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "golden suite failed (exit ${rc}) for ${fixture}")
endif()
