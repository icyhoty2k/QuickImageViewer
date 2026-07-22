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

if (QIV_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()
# ---------------------------------------------

set(SOURCES
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
        src/Renderer/RendererGDI.cpp
        src/Renderer/RendererD2D.cpp
        src/UI/FloatingPanels/FloatingPanelWnd.cpp
        src/UI/FloatingPanels/HelpWnd.cpp
        src/UI/FloatingPanels/ExifWnd.cpp
        src/UI/FloatingPanels/JumpToWnd.cpp
        src/UI/FloatingPanels/StatsWnd.cpp
        src/UI/ThumbnailPanels/CacheWnd.cpp
        src/UI/ThumbnailPanels/DirWnd.cpp
        src/UI/FloatingPanels/HistoryListWnd.cpp
        src/Input/CommandExecuter.cpp
        src/Input/CommandResolver.cpp
        src/Input/AppCommands.cpp
        src/Input/ContextMenuHandler.cpp
        src/CmdArgsIO.cpp
        src/Dedicated/DedicatedInstance.cpp
        src/Dedicated/DedicatedWnd.cpp
        src/Dedicated/DedicatedSettings.cpp
        src/Dedicated/DedicatedLists.cpp
        src/Dedicated/PromotionPlaylist.cpp
        src/UI/UIManager.cpp
        src/Overlays/OverlayManager.cpp
        src/UI/ThumbnailPanels/ThumbnailPanelWnd.cpp
        src/UI/FloatingPanels/FindWnd.cpp
        src/Persistence/HistoryFoldersManager.cpp
        src/CMDArgs.cpp
        src/SlideshowTransitions.cpp
        src/GeoNames.cpp
        src/UI/ThemedDialog.cpp
        libs/lib/tinyexr/include/miniz.c
)

set_source_files_properties(libs/lib/tinyexr/include/miniz.c PROPERTIES COMPILE_FLAGS "/wd4132")

find_program(SCCACHE_PROGRAM sccache)
if (SCCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
endif ()

# WIN32 implicitly handles /SUBSYSTEM:WINDOWS
add_executable(QuickImageViewer WIN32 ${SOURCES})

# --- Enable Link Time Optimization (LTCG) the CMake Way ---
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if (ipo_supported)
    set_target_properties(QuickImageViewer PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
# ----------------------------------------------------------

# 1. Compile Definitions
target_compile_definitions(QuickImageViewer PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        $<$<CONFIG:Release>:NDEBUG>
        OPJ_STATIC
)

# 2. Compile Options
target_compile_options(QuickImageViewer PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor

        # Disable /MP if sccache is active to prevent caching failures
        $<$<NOT:$<BOOL:${SCCACHE_PROGRAM}>>:/MP>

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
target_link_options(QuickImageViewer PRIVATE
        $<$<CONFIG:Release>:/Brepro>         # Reproducible builds
        /MANIFEST:NO                         # Prevent default manifest generation
        $<$<CONFIG:Release>:/INCREMENTAL:NO> # Clean, non-fragmented binary
        $<$<CONFIG:Release>:/OPT:REF>        # Eliminate unreferenced functions/data
        $<$<CONFIG:Release>:/OPT:ICF>        # Fold identical functions
        $<$<CONFIG:Release>:/DEBUG:NONE>     #DO NOT Generate PDB for crash dumps (exe size remains small)
)

# 4. External Libraries (Static Linking: resvg only)
set(STATIC_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/lib/static")

target_include_directories(QuickImageViewer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Dedicated
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Input
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Overlays
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Persistence
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
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
        ws2_32 ntdll userenv # Required by resvg
)

# 7. Track Manifest Changes
set_property(SOURCE resources/resource.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")

# 8. Build Completion Notification
add_custom_command(
        TARGET QuickImageViewer
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "--------------------------------------------------------"
        COMMAND ${CMAKE_COMMAND} -E echo "SUCCESS: Build complete."
        COMMAND powershell -Command "$file = Get-Item '$<TARGET_FILE:QuickImageViewer>'; Write-Host ('Size: {0:N2} KB' -f ($file.Length / 1KB))"
        COMMAND ${CMAKE_COMMAND} -E echo "Directory: $<TARGET_FILE_DIR:QuickImageViewer>"
        COMMAND ${CMAKE_COMMAND} -E echo "--------------------------------------------------------"
        VERBATIM
)
