# Increments the build counter and regenerates BuildNumber.h.
#
# Run as a pre-build step on every build (see the QIV_IncreaseBuildNumber target
# in CMakeLists.txt next to this file), NOT at configure time — the point is that
# pressing Build in the IDE increments the number, whether or not CMake re-ran.
#
# Lives at the project root rather than in a cmake/ subdirectory: it is the only
# build script there is, and a directory holding one file is just another level
# to open.
#
# Inputs (passed with -D):
#   COUNTER_FILE   persistent counter, kept in the SOURCE tree so it survives
#                  wiping the build directory (which is a ramdisk here)
#   OUTPUT_HEADER  generated header, written into the BUILD tree
#   SKIP_MARKER    if this file exists, DO NOT increment — see below
#
# ---------------------------------------------------------------------------
# SKIPPING THE INCREMENT (the "Run" case)
#
# CLion's Run performs a build first, so it is indistinguishable from pressing
# Build — same ninja invocation, same targets. To tell them apart, the Run
# configuration drops a marker file before the build starts; finding one here
# means "this build is only here so the app can be launched", so the number is
# left alone.
#
# The marker is consumed (deleted) whether or not it was used, so it can only
# ever suppress the one build that follows it.
#
# To set it up in CLion:
#   Run ▸ Edit Configurations ▸ your configuration ▸ Before launch
#   ▸ + ▸ Run External tool ▸ +
#       Name:      Skip build-number increment
#       Program:   <CLion>/bin/cmake/win/x64/bin/cmake.exe
#       Arguments: -E touch "$ProjectFileDir$/.qiv_skip_build_number"
#   Then drag that entry ABOVE "Build".
#
#   NOT `cmd /c type nul > file`: CLion hands arguments to the program as
#   separate argv entries rather than through a shell, so the ">" arrives as a
#   literal argument and type reports "cannot find the file specified".
#   `cmake -E touch` needs no shell and no redirection. `cmd /c copy /y nul
#   "<file>"` works for the same reason.
#
# The header is only rewritten when its contents actually change, so a suppressed
# build does not touch it — meaning Version.cpp and resource.rc are not recompiled
# either, and a no-op Run really is a no-op.
# ---------------------------------------------------------------------------

set(_skip FALSE)
if (DEFINED SKIP_MARKER AND EXISTS "${SKIP_MARKER}")
    file(REMOVE "${SKIP_MARKER}") # one marker suppresses exactly one build
    set(_skip TRUE)
endif ()

set(_counter 0)
if (EXISTS "${COUNTER_FILE}")
    file(READ "${COUNTER_FILE}" _counter)
    string(STRIP "${_counter}" _counter)
    if (NOT _counter MATCHES "^[0-9]+$")
        set(_counter 0)
    endif ()
endif ()

if (NOT _skip)
    math(EXPR _counter "${_counter} + 1")
    file(WRITE "${COUNTER_FILE}" "${_counter}\n")
endif ()

# Write via a temp file and copy only if different: an unconditional file(WRITE)
# updates the timestamp even when the text is identical, which would make ninja
# recompile Version.cpp and resource.rc on a suppressed build for no reason.
set(_tmp "${OUTPUT_HEADER}.tmp")
file(WRITE "${_tmp}"
        "// GENERATED — do not edit, do not commit.\n"
        "// Rewritten by IncreaseQIVBuildNumberOnBuild.cmake on every build.\n"
        "#pragma once\n"
        "#define VER_BUILD ${_counter}\n")
file(COPY_FILE "${_tmp}" "${OUTPUT_HEADER}" ONLY_IF_DIFFERENT)
file(REMOVE "${_tmp}")
