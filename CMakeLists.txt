cmake_minimum_required(VERSION 4.2) # 4.2 is likely a typo; 3.20 is standard
project(QuickImageViewer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Source files (Only .cpp and .rc files)
set(SOURCES
        main.cpp
        FileHandler.cpp
        WicDecoder.cpp
        Renderer.cpp
        RegistrySetup.cpp
        DpiAwareInit.cpp
        HelpWindow.cpp
        DropTarget.cpp
        resources/resource.rc
        Renderer/IRenderer.h
        Renderer/RendererGDI.cpp
        Renderer/RendererGDI.h
)

# 2. Add the executable
add_executable(QuickImageViewer WIN32 ${SOURCES})

# Alternatively, the cleanest way to do this in modern CMake:
set_target_properties(QuickImageViewer PROPERTIES
        VS_USER_PROPS "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest"
)

# 4. Define DEBUG_BUILD macro only for Debug
target_compile_definitions(QuickImageViewer PRIVATE
        $<$<CONFIG:Debug>:DEBUG_BUILD>
)

# 5. Link libraries
target_link_libraries(QuickImageViewer PRIVATE
        user32
        comdlg32
        windowscodecs
        gdi32
        dwmapi
        advapi32
        shell32
        ole32       # Needed for Drag and Drop (OleInitialize)
        oleaut32    # Needed for Drag and Drop
        uuid
)