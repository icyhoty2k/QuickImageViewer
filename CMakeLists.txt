cmake_minimum_required(VERSION 4.3)

project(QuickImageViewer LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# --- Custom Option for Static MSVC Runtime ---
#
# OFF, and it cannot simply be switched ON — TESTED 2026-08-06.
#
# /MT would remove msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll from
# every launch (~0.3-1 ms, on the cold path AND on every "open with qIV" while a
# copy is running), and would drop the VC++ redistributable as a dependency —
# "VCRUNTIME140.dll was not found" is a real first-run failure for a portable app.
#
# WHAT BLOCKS IT: the prebuilt libs/lib/openjp2.lib was compiled against the
# DYNAMIC CRT. It references __imp__aligned_malloc / _free / _realloc — the
# __imp_ prefix meaning "imported from a DLL" — which the static CRT does not
# provide, and it carries a /DEFAULTLIB:MSVCRT directive that collides with
# libcmt (LNK4098). resvg.lib links clean; openjp2 is the only blocker.
#
#   openjp2.lib(opj_malloc.c.obj) : error LNK2001: unresolved external symbol
#                                   __imp__aligned_free
#
# THE ONLY REAL FIX is rebuilding OpenJPEG with /MT. Do NOT attempt the
# alias-the-__imp_-symbols trick — it links, and then two allocators disagree
# about which heap a pointer came from, which is a corruption bug that surfaces
# far from its cause.
option(QIV_STATIC_RUNTIME "Statically link the MSVC C/C++ runtime (/MT instead of /MD). Requires an /MT build of openjp2.lib — see above." OFF)

# --- Fast edit-build-run iteration ---------------------------------------
# The link, not the compile, is what a rebuild-to-test loop actually pays for
# here: BuildNumber.h is rewritten every build, so every build relinks, and a
# plain /LTCG relink re-optimizes all ~120 objects from scratch.
#
# ON switches /LTCG to /LTCG:INCREMENTAL, which reoptimizes only the modules
# affected by the edit and reuses cached results for the rest. Whole-program
# optimization stays ON — cross-module inlining, interprocedural register
# allocation and the rest are all still applied, so the runtime characteristics
# of the binary do not change. What is given up is bit-for-bit reproducibility
# (/Brepro), because the linker is now reusing cached codegen.
#
# Not to be confused with /INCREMENTAL, which is a different mechanism and is
# explicitly documented as invalid together with /LTCG.
option(QIV_FAST_ITERATE "Use incremental LTCG (/LTCG:INCREMENTAL) for fast rebuild cycles" OFF)

if (QIV_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()
# ---------------------------------------------

# --- Auto-incrementing build number ----------------------------------------
# Bumped on EVERY build (pressing Build in the IDE), not on CMake reconfigure.
#
# The counter lives in the source tree and IS committed, so the number keeps
# meaning across clones and machines. The generated header lives in the build
# tree and is never committed. Only resource.rc and src/Common/Version.cpp
# include that header — see src/Common/Version.h for why that matters.
set(QIV_BUILD_COUNTER "${CMAKE_CURRENT_SOURCE_DIR}/BuildNumber.txt")
# Dropped by the Run configuration's "Before launch" step so that launching the
# app does not consume a build number — see the notes in the script.
set(QIV_SKIP_MARKER "${CMAKE_CURRENT_SOURCE_DIR}/.qiv_skip_build_number")
set(QIV_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(QIV_BUILD_HEADER "${QIV_GENERATED_DIR}/BuildNumber.h")
file(MAKE_DIRECTORY "${QIV_GENERATED_DIR}")

# Seed it at configure time so the very first build has a header to include.
if (NOT EXISTS "${QIV_BUILD_HEADER}")
    file(WRITE "${QIV_BUILD_HEADER}" "#pragma once\n#define VER_BUILD 0\n")
endif ()

# ALL + no OUTPUT means this target is always considered out of date, which is
# exactly what "increment on every build" requires.
add_custom_target(QIV_IncreaseBuildNumber ALL
        COMMAND ${CMAKE_COMMAND}
        -DCOUNTER_FILE=${QIV_BUILD_COUNTER}
        -DOUTPUT_HEADER=${QIV_BUILD_HEADER}
        -DSKIP_MARKER=${QIV_SKIP_MARKER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/IncreaseQIVBuildNumberOnBuild.cmake
        BYPRODUCTS ${QIV_BUILD_HEADER}
        COMMENT "Incrementing build number"
        VERBATIM)
# ---------------------------------------------------------------------------

set(SOURCES
        src/Common/Version.cpp
        resources/resource.rc
        src/AppMain.cpp
        src/DropTarget.cpp
        src/Input/MouseHandler.cpp
        src/Input/TrayHandler.cpp
        src/WicDecoder.cpp
        src/SvgDecoder.cpp
        src/SimpleFormats.cpp
        src/Platform/DpiAwareInit.cpp
        src/Platform/DriveInfo.cpp
        src/Platform/MonitorInfo.cpp
        src/Platform/FileHandler.cpp
        src/Platform/WriteQueue.cpp
        src/Persistence/RegistryManager.cpp
        src/UI/LinkText.cpp
        src/Persistence/IniFile.cpp
        src/Persistence/RotatingLogFile.cpp
        src/Platform/AppLog.cpp
        src/Persistence/SessionFile.cpp
        src/Renderer/RendererGDI.cpp
        src/Renderer/RendererD2D.cpp
        src/UI/FloatingPanels/FloatingPanelWnd.cpp
        src/UI/FloatingPanels/HelpWnd.cpp
        src/UI/FloatingPanels/ExifWnd.cpp
        src/UI/FloatingPanels/JumpToWnd.cpp
        src/UI/FloatingPanels/ZoomWnd.cpp
        src/UI/FloatingPanels/StatsWnd.cpp
        src/UI/ThumbnailPanels/CacheWnd.cpp
        src/UI/ThumbnailPanels/DirWnd.cpp
        src/UI/FloatingPanels/HistoryListWnd.cpp
        src/Input/CommandExecuter.cpp
        src/Input/CommandResolver.cpp
        src/Input/AppCommands.cpp
        src/CmdArgsIO.cpp
        src/Dedicated/DedicatedInstance.cpp
        src/Dedicated/DedicatedWnd.cpp
        src/Dedicated/DedicatedSettings.cpp
        src/Dedicated/DedicatedLists.cpp
        src/Dedicated/PromotionPlaylist.cpp
        src/Rem_TCP_IP/RemoteSettings.cpp
        src/Rem_TCP_IP/AddressMatch.cpp
        src/Rem_TCP_IP/RemoteBlacklist.cpp
        src/Rem_TCP_IP/RemoteCrypto.cpp
        src/Rem_TCP_IP/RemoteTls.cpp
        src/Rem_TCP_IP/RemoteProtocol.cpp
        src/Rem_TCP_IP/RemoteServer.cpp
        src/Rem_TCP_IP/RemoteBeacon.cpp
        src/Rem_TCP_IP/RemoteExec.cpp
        src/Rem_TCP_IP/RemoteClient.cpp
        src/Rem_TCP_IP/RemoteImageXfer.cpp
        src/UI/GdiPool.cpp
        src/UI/AppMenu/AppMenu.cpp
        src/UI/AppMenu/AppMenuBuilders.cpp
        src/UI/AppMenu/AppMenuSettings.cpp
        src/UI/AppMenu/AppMenuIO.cpp
        src/Rem_TCP_IP/RemoteWnd.cpp
        src/Rem_TCP_IP/RemoteClientsWnd.cpp
        src/Rem_TCP_IP/RemoteMirror.cpp
        src/Rem_TCP_IP/MirrorPickerWnd.cpp
        src/Rem_TCP_IP/RemoteLog.cpp
        src/Rem_TCP_IP/RemoteLogWnd.cpp
        src/Rem_TCP_IP/RemoteCmdWnd.cpp
        src/Rem_TCP_IP/RemotesFile.cpp
        src/Rem_TCP_IP/RemotesWnd.cpp
        src/UI/UIManager.cpp
        src/Overlays/OverlayManager.cpp
        src/UI/ThumbnailPanels/ThumbnailPanelWnd.cpp
        src/UI/FloatingPanels/FindWnd.cpp
        src/Persistence/HistoryFoldersManager.cpp
        src/CMDArgs.cpp
        src/SlideshowTransitions.cpp
        src/GeoNames.cpp
        src/UI/ThemedDialog.cpp
        src/UI/ThemedTooltip.cpp
        src/Platform/CrashHandler.cpp
        # miniz, vendored in its own right — see libs/lib/miniz/README.md.
        # It used to be the single amalgamated copy bundled inside tinyexr,
        # which meant it could not be updated without updating tinyexr too.
        libs/lib/miniz/miniz.c
        libs/lib/miniz/miniz_tdef.c
        libs/lib/miniz/miniz_tinfl.c
        libs/lib/miniz/miniz_zip.c
        src/Common/Converters.cpp
)

# C4132: 'const object should be initialized' — upstream miniz, not ours to fix.
set_source_files_properties(
        libs/lib/miniz/miniz.c
        libs/lib/miniz/miniz_tdef.c
        libs/lib/miniz/miniz_tinfl.c
        libs/lib/miniz/miniz_zip.c
        PROPERTIES COMPILE_FLAGS "/wd4132")

#find_program(SCCACHE_PROGRAM sccache)
#if (SCCACHE_PROGRAM)
#    set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
#    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
#endif ()

# WIN32 implicitly handles /SUBSYSTEM:WINDOWS
add_executable(QuickImageViewer WIN32 ${SOURCES})

# Guarantees the counter is bumped and the header rewritten BEFORE anything that
# includes it is compiled.
add_dependencies(QuickImageViewer QIV_IncreaseBuildNumber)

# --- Precompiled header ---------------------------------------------------
# MSVC consumes this via /FI (force include), so NO source file includes it by
# hand — see the header itself for what may and may not go in it.
# The COMPILE_LANGUAGE:CXX guard keeps miniz.c (the only C file) out of it; a
# C++ PCH cannot be applied to a C translation unit.
target_precompile_headers(QuickImageViewer PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h>"
)
# ---------------------------------------------------------------------------

# --- Link Time Code Generation, spelled out ---------------------------------
# Deliberately NOT set_target_properties(... INTERPROCEDURAL_OPTIMIZATION TRUE).
# That property emits /GL plus a bare /LTCG, and there is no supported way to
# make it emit /LTCG:INCREMENTAL instead — appending a second /LTCG:... would
# leave two conflicting forms on the linker command line. Both halves are
# written out here so the flag that reaches link.exe is the flag in this file.
#
# /GL on the compile side is what makes the objects eligible; the linker form
# below decides how much of that work is redone on each link.
target_compile_options(QuickImageViewer PRIVATE $<$<CONFIG:Release>:/GL>)
# ----------------------------------------------------------------------------

# 1. Compile Definitions
target_compile_definitions(QuickImageViewer PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        $<$<CONFIG:Release>:NDEBUG>
        OPJ_STATIC
)

# DO NOT add MINIZ_NO_STDIO here. It was tried in order to compile miniz's
# file-based zip functions out of the build — qIV calls only the in-memory ones,
# so on paper nothing was lost — and settings IMPORT crashed at runtime rather
# than failing to build.
#
# The likely mechanism is the one that makes this flag dangerous at all: it
# changes what miniz.h declares, miniz.h is included by three C++ translation
# units as well as by miniz.c, and any object file left over from a previous
# configuration then disagrees with the rest about the library it was compiled
# against. A mismatch of that kind does not produce a compiler error; it
# produces a crash somewhere else entirely.
#
# If it is ever revisited, it must be from a CLEAN build directory, and settings
# export and import must both be exercised before it is trusted. The CodeQL
# finding it was meant to silence — a time-of-check time-of-use race in
# mz_zip_add_mem_to_archive_file_in_place_v2 — is not reachable from qIV, so
# there is nothing here worth a second crash.
#
# PROPOSED AGAIN 2026-08-06 and rejected again on reading this note. Verified
# once more at that point: the flagged function opens an archive by PATH, and
# the only miniz callers in this project are in-memory —
#   tinyexr        mz_compress / mz_uncompress
#   AppMenuIO.cpp  heap zip writer/reader (Backup History & Favorites)
#   GeoNames.cpp   mz_uncompress
# Dismiss the CodeQL alert as unreachable instead. The same applies to the
# "redundant null check" alert in tinfl_decompress, which is present in upstream
# miniz master and therefore is not fixed by updating either.
#
# Still true after miniz moved to libs/lib/miniz (11.3.2, was a copy bundled
# inside tinyexr): miniz.h is included by three C++ translation units plus
# miniz's own sources, which is exactly the condition that made the flag
# dangerous.

# 2. Compile Options
#
# /MP tells ONE cl.exe invocation to fan out over several source files. Under
# Ninja (and Make) the generator already runs N compiles in parallel, so /MP
# multiplies that by another N and the machine ends up oversubscribed. It is
# only a win with the Visual Studio generator, which hands cl.exe a whole file
# list in a single invocation.
if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set(QIV_MP_FLAG /MP)
else ()
    set(QIV_MP_FLAG "")
endif ()

target_compile_options(QuickImageViewer PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        ${QIV_MP_FLAG} # /MP, but only under the Visual Studio generator — see above

        # --- Explicit Release Performance Overrides ---
        $<$<CONFIG:Release>:/O2>         # Maximize speed (implies /Ot)
        $<$<CONFIG:Release>:/Oi>         # Enable intrinsic functions
        $<$<CONFIG:Release>:/fp:fast>    # Fast floating-point math
        $<$<CONFIG:Release>:/arch:AVX2>  # Hardware acceleration
        $<$<CONFIG:Release>:/Gw>         # Optimize global data
        $<$<CONFIG:Release>:/Gy>         # Function-level linking
        $<$<CONFIG:Release>:/GF>         # Pool identical string literals
        $<$<CONFIG:Release>:/Zc:inline>  # Remove unreferenced internal functions
        $<$<CONFIG:Release>:/Zc:externConstexpr>
        $<$<CONFIG:Release>:/favor:blend># Blend AMD and Intel
        $<$<CONFIG:Release>:/GS>         # Enable buffer-overrun checks
)

# 3. Explicit Linker Flags
#
# /OPT:REF and /OPT:ICF stay on in BOTH configurations — they are image-level
# passes, cheap next to code generation, and dropping them would change the
# binary's layout and size. Only the LTCG form differs between the two.
target_link_options(QuickImageViewer PRIVATE
        /MANIFEST:NO                         # Prevent default manifest generation
        # /DEBUG:FULL, not /DEBUG:NONE — and this does NOT grow the exe.
        #
        # A PDB is a separate file. The linker adds only a small debug directory
        # entry naming it; code generation, optimisation and layout are unchanged,
        # so the shipped binary is the same one either way. The PDB is simply not
        # distributed.
        #
        # WHY IT IS NEEDED. src/Platform/CrashHandler.cpp writes a minidump when
        # qIV dies on a user's machine. Without a PDB from the EXACT build that
        # produced the crash, that dump resolves to nothing but raw addresses —
        # which is what happened on 2026-08-05, when four dumps from a real crash
        # could not be read. A dump nobody can symbolise is a file, not evidence.
        #
        # KEEP THE PDB WITH EACH RELEASE. It is only usable against the one binary
        # it was linked with; a PDB from any other build maps to the wrong lines,
        # silently. The makeRelease flow already archives the Android R8 mapping
        # for the same reason.
        $<$<CONFIG:Release>:/DEBUG:FULL>
        $<$<CONFIG:Release>:/INCREMENTAL:NO> # /INCREMENTAL is invalid with /LTCG
        $<$<CONFIG:Release>:/OPT:REF>        # Eliminate unreferenced functions/data
        $<$<CONFIG:Release>:/OPT:ICF>        # Fold identical functions

        # --- DELAY-LOADED, and this is a STARTUP optimisation ----------------
        #
        # These six are statically imported, so the Windows loader maps every one
        # of them — and their dependency chains — before wWinMain runs. The
        # graphics and imaging stack is the expensive part of that.
        #
        # THE PATH THIS IS FOR: "open with qIV" while a copy is already running.
        # That launch parses a command line, reads an .ini, takes a mutex, finds a
        # window, sends WM_COPYDATA and exits. It draws nothing, decodes nothing
        # and opens no dialog — so mapping Direct2D, D3D11, DXGI, DirectWrite and
        # WIC for it is pure loader work on the one path a user is watching. It is
        # also the most common way this program is started.
        #
        # The surviving instance pays the same cost, only later and at first use,
        # where it overlaps window creation instead of preceding it.
        #
        # SAFE because all six are OS components present on every supported
        # Windows, and because a delay-load only changes WHEN a missing DLL is
        # reported: at first call rather than at launch. Nothing here is optional
        # or redistributable.
        #
        # Do NOT add ws2_32, bcrypt, secur32, crypt32 or ncrypt to this list
        # without checking Rem_TCP_IP first — the listener can be started by
        # autostart before the window is up, and a delay-load fault on a socket
        # thread is a far worse failure than the microseconds it would save.
        # dxgi.dll and windowscodecs.dll are deliberately NOT here. They are
        # never statically imported — WIC arrives through
        # CoCreateInstance(CLSID_WICImagingFactory) and DXGI transitively off the
        # D3D11 device, both COM activations that already load on demand. Listing
        # them did nothing except produce, on every link:
        #
        #     LINK : warning LNK4199: /DELAYLOAD:dxgi.dll ignored;
        #                             no imports found from dxgi.dll
        #
        # A warning nobody can act on is one everybody learns to ignore, which is
        # how a real one gets missed.
        $<$<CONFIG:Release>:/DELAYLOAD:d2d1.dll>
        $<$<CONFIG:Release>:/DELAYLOAD:d3d11.dll>
        $<$<CONFIG:Release>:/DELAYLOAD:dwrite.dll>
        $<$<CONFIG:Release>:/DELAYLOAD:comdlg32.dll>
)

# The helper that resolves a delay-loaded import on its first call. Required by
# /DELAYLOAD above; without it the link fails on __delayLoadHelper2.
target_link_libraries(QuickImageViewer PRIVATE delayimp)

if (QIV_FAST_ITERATE)
    # Reoptimize only the modules the edit touched. Whole-program optimization
    # is still performed; the linker caches per-module results in a .iobj beside
    # the exe. /Brepro is omitted because that cache defeats bit-reproducibility.
    target_link_options(QuickImageViewer PRIVATE
            $<$<CONFIG:Release>:/LTCG:INCREMENTAL>
    )
else ()
    target_link_options(QuickImageViewer PRIVATE
            $<$<CONFIG:Release>:/LTCG>   # Full whole-program optimization every link
            $<$<CONFIG:Release>:/Brepro> # Reproducible builds
    )
endif ()

# 4. External Libraries (Static Linking: resvg only)
set(STATIC_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/static")

# --- One vendored .lib per CRT, so the switch is just the option -------------
#
# A static .lib records which CRT it was built against, so a /MD library cannot
# be linked into an /MT binary — see the QIV_STATIC_RUNTIME option at the top of
# this file for the exact errors. The answer is to keep BOTH builds in the repo
# and pick by the option, rather than making the switch depend on a tool being
# installed or an environment variable being set.
#
#     libs/lib/static/     — /MD  (the default)
#     libs/lib/static-mt/  — /MT
#
# HEADERS ARE SHARED, which is only sound while both libraries are the SAME
# UPSTREAM VERSION. The vendored headers are OpenJPEG 2.5.4
# (libs/lib/openjpeg/include/opj_config.h), so an /MT openjp2.lib built from any
# other version compiles against one set of structs and links against another —
# an ABI mismatch that builds cleanly and corrupts memory at run time. If the
# /MT copy ever has to be a different version, its headers must be vendored
# beside it and selected here too.
#
# Falls back per library rather than per directory: resvg.lib links clean under
# /MT today, so it is not duplicated. If that ever changes, drop an /MT build
# into static-mt/ and this picks it up with no edit.
set(STATIC_MT_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/static-mt")

function(qiv_pick_static_lib out_var libName)
    if (QIV_STATIC_RUNTIME AND EXISTS "${STATIC_MT_LIBS_DIR}/${libName}")
        set(${out_var} "${STATIC_MT_LIBS_DIR}/${libName}" PARENT_SCOPE)
    else ()
        set(${out_var} "${STATIC_LIBS_DIR}/${libName}" PARENT_SCOPE)
    endif ()
endfunction()

qiv_pick_static_lib(QIV_OPENJP2_LIB openjp2.lib)
qiv_pick_static_lib(QIV_RESVG_LIB   resvg.lib)

# Caught at CONFIGURE time with an actionable message, rather than as three
# unresolved __imp_ symbols at the end of a full link.
if (QIV_STATIC_RUNTIME AND NOT EXISTS "${STATIC_MT_LIBS_DIR}/openjp2.lib")
    message(FATAL_ERROR
            "QIV_STATIC_RUNTIME is ON, but libs/lib/static-mt/openjp2.lib is missing.\n"
            "  The /MD copy in libs/lib/static/ cannot link into an /MT binary.\n"
            "\n"
            "  Produce one — it must be OpenJPEG 2.5.4 to match the vendored headers:\n"
            "      vcpkg install openjpeg:x64-windows-static\n"
            "      copy <vcpkg>/installed/x64-windows-static/lib/openjp2.lib\n"
            "           libs/lib/static-mt/openjp2.lib\n"
            "  (check the vcpkg version first: vcpkg list openjpeg)\n"
            "\n"
            "  Or build it: cmake -DBUILD_SHARED_LIBS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded\n"
            "\n"
            "  Or turn the option off: -DQIV_STATIC_RUNTIME=OFF")
endif ()

message(STATUS "openjp2: ${QIV_OPENJP2_LIB}")

target_include_directories(QuickImageViewer PRIVATE
        ${QIV_GENERATED_DIR} # BuildNumber.h — regenerated every build
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Dedicated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Input
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Overlays
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Persistence
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Rem_TCP_IP
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UI
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/resvg/include
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/openjpeg/include
        # BEFORE tinyexr's directory. tinyexr does #include <miniz.h>, and the
        # old bundled copy used to live beside tinyexr.h — this ordering is what
        # makes that resolve to the vendored miniz rather than to a stale
        # duplicate, if one is ever restored by accident.
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/miniz
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/tinyexr/include
)

target_link_directories(QuickImageViewer PRIVATE
        ${STATIC_LIBS_DIR}
)

# 5. External Codec Libraries
#
# FULL PATHS, chosen above by CRT. A bare "openjp2.lib" would be resolved through
# target_link_directories, which cannot tell the two variants apart.
target_link_libraries(QuickImageViewer PRIVATE
        ${QIV_RESVG_LIB}
        ${QIV_OPENJP2_LIB}
)

# 6. Windows Libraries
target_link_libraries(QuickImageViewer PRIVATE
        d3d11 dxgi d2d1 dwrite user32 comdlg32 windowscodecs gdi32 dwmapi
        uxtheme advapi32 shell32 ole32 oleaut32 uuid shlwapi shcore
        ws2_32 ntdll userenv # ws2_32 also carries the Rem_TCP_IP sockets
        bcrypt               # CNG — remote-control password hashing / HMAC
        secur32 crypt32 ncrypt # Schannel TLS + the self-signed server certificate

        # These two were reaching the linker through #pragma comment(lib, ...) in
        # RendererD2D.cpp and AppCommands.cpp instead of from here, so this file
        # was not actually the full dependency list it appears to be. Moved so it
        # is — RemoteCrypto.cpp and RemoteTls.cpp already say in their own
        # comments that CMake owns library configuration, and the renderer was
        # the one place still disagreeing.
        dxguid               # the D3D/DXGI GUIDs RendererD2D references
        comctl32             # common controls — AppCommands
)

# 7. Track Manifest Changes
set_property(SOURCE resources/resource.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")

# 8. Build Completion Notification
add_custom_command(
        TARGET QuickImageViewer
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--------------------------------------------------------"
        COMMAND ${CMAKE_COMMAND} -E echo "SUCCESS: Build complete."
        # Size and version in ONE powershell call — spawning a second process just
        # to print a line costs more than everything else in this block. The
        # version is read back out of the linked exe's own resource, so it is the
        # value that actually shipped, build number included.
        COMMAND powershell -Command "$file = Get-Item '$<TARGET_FILE:QuickImageViewer>'; Write-Host ('Size: {0:N2} KB' -f ($file.Length / 1KB)); Write-Host ('Version: {0}' -f $file.VersionInfo.FileVersion)"
        COMMAND ${CMAKE_COMMAND} -E echo "Directory: $<TARGET_FILE_DIR:QuickImageViewer>"
        COMMAND ${CMAKE_COMMAND} -E echo "--------------------------------------------------------"
        VERBATIM
)

# =============================================================================
# Unit tests — a SECOND, tiny target. `ctest` in the build directory runs it.
#
# Separate executable rather than a flag on the main one: the viewer is a WIN32
# subsystem app with a wWinMain and a global AppState, and a test binary needs
# neither. Linking only the translation units under test also keeps the suite
# honest — if a test needs half the program to link, that is the design telling
# you the thing under test is not a unit.
#
# ON by default, because a suite nobody runs catches nothing. It compiles two
# small files and adds well under a second to a build. Turn it off with
# -DQIV_BUILD_TESTS=OFF if it is ever in the way.
# =============================================================================
option(QIV_BUILD_TESTS "Build the qIV unit tests" ON)

if (QIV_BUILD_TESTS)
    enable_testing()

    add_executable(qivTests
            test/qivTests.cpp
            src/Common/Converters.cpp   # the only .cpp the tests need; the rest are header-only
            # The logging trio. They link here because none of them reaches into
            # AppState, a window or a socket — RemoteLog needs only IniFile (for
            # the folder beside the exe) and RotatingLogFile (for the writer).
            # Rotation and format round-tripping are exactly the kind of thing
            # that breaks without any visible symptom.
            src/Persistence/IniFile.cpp
            src/Persistence/RotatingLogFile.cpp
            src/Rem_TCP_IP/RemoteLog.cpp
            # The AllowList matcher — the most security-relevant pure logic in
            # the program. It was split out of RemoteSettings.cpp precisely so it
            # could be linked here; that file also pulls in AppState,
            # RemoteBlacklist, RemoteCrypto and DedicatedSettings.
            src/Rem_TCP_IP/AddressMatch.cpp
            # The password hash and the handshake HMAC. If this is wrong,
            # authentication is wrong — and unlike a decoder it fails silently in
            # the dangerous direction. It reaches nothing but bcrypt and
            # Constants.h, so it costs one system import library to cover.
            src/Rem_TCP_IP/RemoteCrypto.cpp
            # Constants::APP_VERSION is declared extern in Constants.h and
            # DEFINED here — RemoteLog's file preamble names the build that wrote
            # the log, so the suite needs the definition or it fails to link.
            src/Common/Version.cpp
    )

    target_include_directories(qivTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/src/Common
            # BuildNumber.h, which Version.cpp includes. Generated into the build
            # tree and rewritten on every build, so it is not beside the source.
            ${QIV_GENERATED_DIR}
    )

    # NO precompiled header, and no /FI pch.h. The main target force-includes
    # pch.h, which pulls in the whole Win32 and Direct2D surface; the tests need
    # none of it and would then fail to build for reasons that have nothing to do
    # with what they are testing.
    target_compile_definitions(qivTests PRIVATE NOMINMAX UNICODE _UNICODE)

    # The menu-id tests READ AppMenuIds.h and the two dispatch files at run time
    # rather than duplicating the id list in the test. A hand-copied list is the
    # very thing they exist to catch: it would agree with itself while the header
    # drifted. Parsing the real header means an id added tomorrow is covered
    # without anyone remembering to update a test.
    target_compile_definitions(qivTests PRIVATE
            QIV_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

    # /utf-8 — REQUIRED, not cosmetic, and the main target has had it all along.
    #
    # These sources are UTF-8 and contain non-ASCII character constants, e.g.
    # RemoteLog.cpp's L'·' (the substitute for an unprintable byte in a logged
    # line). Without /utf-8 MSVC reads the file in the system codepage, so the
    # two UTF-8 bytes of '·' arrive as two characters and the compile fails with
    #
    #     error C2015: too many characters in constant
    #
    # It surfaced the moment RemoteLog.cpp was added to this target, in CI rather
    # than locally, because the main target's flags never applied here. Any file
    # added below inherits the same requirement.
    target_compile_options(qivTests PRIVATE /utf-8)

    # ws2_32 — InetPtonW and ntohl, which AddressMatch.cpp uses to turn
    # "192.168.0.0/24" into numbers.
    #
    # A SYSTEM import library, not another slice of the program: the rule this
    # target lives by is about not linking half the APPLICATION, and an OS import
    # lib costs nothing and hides nothing.
    target_link_libraries(qivTests PRIVATE ws2_32 bcrypt)

    add_test(NAME qivTests COMMAND qivTests)
endif ()
