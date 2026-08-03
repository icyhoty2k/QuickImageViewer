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
option(QIV_STATIC_RUNTIME "Statically link the MSVC C/C++ runtime (/MT instead of /MD)" OFF)

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
        src/Platform/FileHandler.cpp
        src/Platform/WriteQueue.cpp
        src/Persistence/RegistryManager.cpp
        src/UI/LinkText.cpp
        src/Persistence/IniFile.cpp
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
        src/Rem_TCP_IP/RemoteBlacklist.cpp
        src/Rem_TCP_IP/RemoteCrypto.cpp
        src/Rem_TCP_IP/RemoteTls.cpp
        src/Rem_TCP_IP/RemoteProtocol.cpp
        src/Rem_TCP_IP/RemoteServer.cpp
        src/Rem_TCP_IP/RemoteExec.cpp
        src/Rem_TCP_IP/RemoteClient.cpp
        src/Rem_TCP_IP/RemoteImageXfer.cpp
        src/UI/GdiPool.cpp
        src/UI/AppMenu/AppMenu.cpp
        src/UI/AppMenu/AppMenuBuilders.cpp
        src/UI/AppMenu/AppMenuSettings.cpp
        src/UI/AppMenu/AppMenuIO.cpp
        src/Rem_TCP_IP/RemoteWnd.cpp
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
        libs/lib/tinyexr/include/miniz.c
        src/Common/Converters.cpp
)

set_source_files_properties(libs/lib/tinyexr/include/miniz.c PROPERTIES COMPILE_FLAGS "/wd4132")

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
        $<$<CONFIG:Release>:/DEBUG:NONE>     # DO NOT Generate PDB (exe size remains small)
        $<$<CONFIG:Release>:/INCREMENTAL:NO> # /INCREMENTAL is invalid with /LTCG
        $<$<CONFIG:Release>:/OPT:REF>        # Eliminate unreferenced functions/data
        $<$<CONFIG:Release>:/OPT:ICF>        # Fold identical functions
)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/tinyexr/include
)

target_link_directories(QuickImageViewer PRIVATE
        ${STATIC_LIBS_DIR}
)

# 5. External Codec Libraries
target_link_libraries(QuickImageViewer PRIVATE
        resvg.lib
        openjp2.lib
)

# 6. Windows Libraries
target_link_libraries(QuickImageViewer PRIVATE
        d3d11 dxgi d2d1 dwrite user32 comdlg32 windowscodecs gdi32 dwmapi
        uxtheme advapi32 shell32 ole32 oleaut32 uuid shlwapi shcore
        ws2_32 ntdll userenv # ws2_32 also carries the Rem_TCP_IP sockets
        bcrypt               # CNG — remote-control password hashing / HMAC
        secur32 crypt32 ncrypt # Schannel TLS + the self-signed server certificate
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
