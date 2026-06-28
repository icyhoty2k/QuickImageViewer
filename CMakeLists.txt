cmake_minimum_required(VERSION 3.20)
project(QuickImageViewer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Matches your exact physical file tree
set(SOURCES
        src/AppMain.cpp
        src/DropTarget.cpp
        src/MouseHandler.cpp
        src/WicDecoder.cpp
        src/Platform/DpiAwareInit.cpp
        src/Platform/FileHandler.cpp
        src/Platform/RegistrySetup.cpp
        src/Renderer/RendererGDI.cpp
        src/Renderer/RendererD2D.cpp
        src/Cache/ImageCache.cpp
        src/UI/HelpWindow.cpp
        resources/resource.rc
)

# 1. Define the executable first
add_executable(QuickImageViewer WIN32 ${SOURCES})

# 2. NOW define the definitions for that target
target_compile_definitions(QuickImageViewer PRIVATE
        NOMINMAX
        $<$<CONFIG:Debug>:_DEBUG>
        $<$<CONFIG:Release>:NDEBUG>
)

# 3. Link libraries
target_link_libraries(QuickImageViewer PRIVATE
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
)

# Ensure the compiler can resolve includes cleanly
target_include_directories(QuickImageViewer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Cache
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UI
)