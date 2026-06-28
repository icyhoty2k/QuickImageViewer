cmake_minimum_required(VERSION 3.20)

project(QuickImageViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(QuickImageViewer WIN32 ${SOURCES})

# 1. Compile definitions
target_compile_definitions(QuickImageViewer PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
)

# 2. Strict warnings for code quality
target_compile_options(QuickImageViewer PRIVATE
        /W4
        /permissive-
        /EHsc
)

# 3. Include directories
target_include_directories(QuickImageViewer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Cache
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Platform
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Renderer
        ${CMAKE_CURRENT_SOURCE_DIR}/src/UI
)

# 4. Windows libraries
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

# 5. Robust manifest embedding
add_custom_command(TARGET QuickImageViewer POST_BUILD
        COMMAND mt.exe
        /manifest "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
        "/outputresource:$<TARGET_FILE:QuickImageViewer>;#1"
        COMMENT "Embedding custom manifest via mt.exe..."
        VERBATIM
)
