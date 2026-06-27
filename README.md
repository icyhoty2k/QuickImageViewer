## Preview
![Application Screenshot](Screenshot_App.png)
## Preview
![Settings Screenshot](Screenshot_Shortcuts.png)
QuickImageViewer
A lightweight, high-performance image viewer built for speed and direct control. Designed for users who prefer lean, efficient code without unnecessary abstractions.

Key Features
Performance-Focused: Leverages native Windows Imaging Component (WIC) for fast decoding.

No-Magic Architecture: Modular, explicit C++ design for maintainability and direct control.

Open: Licensed under AGPLv3 to ensure the project and its future improvements remain free and open to everyone.

Building the Project
This project uses CMake. To build it, ensure you have a C++ compiler (like MSVC or MinGW) and CMake installed.

Clone the repository:

Bash
git clone https://github.com/icyhoty2k/QuickImageViewer.git
Navigate to the project folder:

Bash
cd QuickImageViewer
Build with CMake:

Bash
mkdir build
cd build
cmake ..
cmake --build .
Project Structure
main.cpp: Entry point and application lifecycle management.

WicDecoder.cpp: Native WIC-based image decoding logic.

Renderer.cpp: Viewport rendering and display operations.

FileHandler.cpp: Low-level filesystem and I/O management.

AppState.h: Centralized application state.

Contributing
Contributions are welcome! Please see CONTRIBUTING.md for guidelines on how to submit features and fixes.

License
This project is licensed under the GNU Affero General Public License v3.0 (AGPLv3). See the LICENSE file for details.
