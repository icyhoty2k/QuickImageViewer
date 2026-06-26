cmake_minimum_required(VERSION 3.20) # Corrected version
project(QuickImageViewer)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Source files (Only .cpp and .rc files)
# DO NOT include headers (.h) in set(SOURCES ...).
# CMake finds them automatically.
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
)

# 2. Add the executable
add_executable(QuickImageViewer WIN32 ${SOURCES})

# 3. Link libraries (Ensure d2d1 and dwrite are here)
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