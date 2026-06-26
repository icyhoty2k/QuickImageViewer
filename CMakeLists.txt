cmake_minimum_required(VERSION 4.2)
project(QuickImageViewer)

set(CMAKE_CXX_STANDARD 23)

enable_language(RC) # <-- FIX 1: Tell CMake to compile resource files

# Force a GUI application rather than a console window
add_executable(QuickImageViewer WIN32
        main.cpp
        FileHandler.cpp
        WicDecoder.cpp
        Renderer.cpp
        AppState.h
        FileHandler.h
        WicDecoder.h
        Renderer.h
        app.manifest
        DpiAwareInit.cpp
        DpiAwareInit.h
        Constants.h
        resources/resource.h
        resources/resource.rc

)

# Link the target to WIC and GDI32 instead of GDI+
# 2. Add dwmapi to target_link_libraries
target_link_libraries(QuickImageViewer PRIVATE
        user32
        comdlg32
        windowscodecs
        gdi32
        dwmapi
)