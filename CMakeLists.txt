cmake_minimum_required(VERSION 3.20)
project(QuickImageViewer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
        main.cpp
        FileHandler.cpp
        WicDecoder.cpp
        RegistrySetup.cpp
        DpiAwareInit.cpp
        HelpWindow.cpp
        DropTarget.cpp
        resources/resource.rc
        Renderer/RendererGDI.cpp
        Renderer/RendererD2D.cpp
        MouseHandler.h
        mouseHandler.cpp
)

# 1. Define the executable first
add_executable(QuickImageViewer WIN32 ${SOURCES})

# 2. NOW define the definitions for that target
target_compile_definitions(QuickImageViewer PRIVATE
        $<$<CONFIG:Debug>:DEBUG_BUILD>
        NOMINMAX
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