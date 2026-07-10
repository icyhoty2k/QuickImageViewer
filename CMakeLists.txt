cmake_minimum_required(VERSION 3.20)

project(QuickImageViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Strictly enforce standard C++23 without compiler-specific deviations

# --- Custom Option for Static MSVC Runtime ---
option(QIV_STATIC_RUNTIME "Statically link the MSVC C/C++ runtime (/MT instead of /MD)" OFF)

if (QIV_STATIC_RUNTIME)
    # Statically link the runtime (/MT for Release, /MTd for Debug)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    # Dynamically link the runtime (/MD for Release, /MDd for Debug)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()
# ---------------------------------------------

set(SOURCES
        resources/resource.rc
        src/AppMain.cpp
        src/DropTarget.cpp
        src/Input/MouseHandler.cpp
        src/WicDecoder.cpp
        src/SvgDecoder.cpp
        src/Platform/DpiAwareInit.cpp
        src/Platform/DriveInfo.cpp
        src/Platform/FileHandler.cpp
        src/Platform/RegistrySetup.cpp
        src/Renderer/RendererGDI.cpp
        src/Renderer/RendererD2D.cpp
        src/UI/HelpWnd.cpp
        src/UI/CacheWnd.cpp
        src/UI/DirWnd.cpp
        src/UI/HistoryListWnd.cpp
        src/Input/CommandExecuter.cpp
        src/Input/CommandResolver.cpp
        src/Input/AppCommands.cpp
        src/UI/UIManager.cpp
        src/Overlays/OverlayManager.cpp
        src/UI/ThumbnailPanelWnd.cpp
        src/Persistence/HistoryFoldersManager.cpp
)
find_program(SCCACHE_PROGRAM sccache)
if (SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
endif ()

add_executable(QuickImageViewer WIN32 ${SOURCES})

# 1. Compile Definitions
target_compile_definitions(QuickImageViewer PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        $<$<CONFIG:Release>:NDEBUG> # Standard macro to strip assert() overhead in Release
)

# 2. Compile Options (Warnings, MP, and Release Optimization)
target_compile_options(QuickImageViewer PRIVATE
        /W4
        #        /analyze
        /permissive-
        /EHsc
        /MP
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        # --- Explicit Release Performance Overrides ---
        $<$<CONFIG:Release>:/O2>         # Maximize speed
        $<$<CONFIG:Release>:/Oi>         # Enable intrinsic functions
        $<$<CONFIG:Release>:/Ot>         # Favor fast code over small size
        $<$<CONFIG:Release>:/fp:fast>    # Fast floating-point math for UI/viewport transforms
        $<$<CONFIG:Release>:/arch:AVX2>  # Hardware acceleration for modern CPUs
        $<$<CONFIG:Release>:/GL>         # Whole Program Optimization (LTCG part 1)
        $<$<CONFIG:Release>:/Gw>         # Optimize global data
        $<$<CONFIG:Release>:/Gy>         # Function-level linking.
        $<$<CONFIG:Release>:/GF>         # Pool identical string literals. Small binary improvement. No downside.
        $<$<CONFIG:Release>:/Zc:inline>         # Optimize global data
        $<$<CONFIG:Release>:/favor:AMD64>         # Optimize global data


        # $<$<CONFIG:Release>:/GS->      # Uncomment to disable stack checks for absolute max speed
)

# 3. Explicit Linker Flags (Safe & Industry Standard)
target_link_options(QuickImageViewer PRIVATE
        /SUBSYSTEM:WINDOWS                     # Explicitly target Win32 subsystem to prevent console windows
        /MANIFEST:NO                           # Prevent default manifest generation i provide my own in resources/app.manifest
        $<$<CONFIG:Release>:/LTCG>             # Link Time Code Generation
        $<$<CONFIG:Release>:/INCREMENTAL:NO>   # Clean, non-fragmented binary
        $<$<CONFIG:Release>:/OPT:REF>          # Eliminate unreferenced functions/data
        $<$<CONFIG:Release>:/OPT:ICF>          # Fold identical functions to reduce executable size
        #        $<$<CONFIG:Release>:/CGTHREADS:16>      # Multi-thread the LTCG code generation phase
)

# 4. Include Directories
target_include_directories(QuickImageViewer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UI
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Input
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Persistence
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Overlays
)

# 5. Windows Libraries
target_link_libraries(QuickImageViewer PRIVATE
        d3d11
        dxgi
        d2d1
        dwrite
        user32
        comdlg32
        windowscodecs
        gdi32
        dwmapi
        advapi32
        shell32
        ole32
        oleaut32
        uuid
        shlwapi
)

# 6. Track Manifest Changes
# This tells CMake that if app.manifest changes, resource.rc needs to be recompiled.
set_property(SOURCE resources/resource.rc PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.manifest")

# Build Completion Notification
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
