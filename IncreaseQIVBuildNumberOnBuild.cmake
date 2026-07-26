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
#                  wiping the build directory
#   OUTPUT_HEADER  generated header, written into the BUILD tree
#
# The header is deliberately tiny and is included by exactly two things
# (resource.rc and Version.cpp). Putting the number anywhere Constants.h can see
# it would invalidate every translation unit on every build.

set(_counter 0)
if (EXISTS "${COUNTER_FILE}")
    file(READ "${COUNTER_FILE}" _counter)
    string(STRIP "${_counter}" _counter)
    if (NOT _counter MATCHES "^[0-9]+$")
        set(_counter 0)
    endif ()
endif ()

math(EXPR _counter "${_counter} + 1")
file(WRITE "${COUNTER_FILE}" "${_counter}\n")

file(WRITE "${OUTPUT_HEADER}"
        "// GENERATED — do not edit, do not commit.\n"
        "// Rewritten by IncreaseQIVBuildNumberOnBuild.cmake on every build.\n"
        "#pragma once\n"
        "#define VER_BUILD ${_counter}\n")
