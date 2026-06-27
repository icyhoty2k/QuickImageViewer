# 🖼️ QuickImageViewer (QIV)

A high-performance, ultra-lightweight image viewer designed for speed and direct hardware control. Built with native C++ and Win32 APIs, QIV delivers 90%+ feature parity with legacy viewers at a fraction of the footprint (176 KB) while supporting modern formats like WebP and JPEG XL.

---

## 📸 Preview

| Main Interface | Shortcuts & Integration |
| :--- | :--- |
| ![Application](Screenshot_App.png) | ![Shortcuts](Screenshot_Shortcuts.png) |

---

## 🚀 Download
You can download the latest version of the viewer from the **[Releases page](https://github.com/icyhoty2k/PosMan/releases/latest)**.

> *Built with native C++ for maximum efficiency (176 KB).*

---

## ✨ Features
* **⚡ Extreme Efficiency:** 176 KB footprint with near-instant startup.
* **👻 Service-Like Persistence:** Stays resident in RAM and "ghosts" into the background (Esc to hide), ready for instant recall.
* **🩹 Self-Healing:** Automatically manages its own registry paths and file associations—just run it once after moving to any location.
* **🛠️ Zero-Magic Design:** Native WIC-based decoding, explicit state management, and direct Windows API calls for maximum transparency and speed.
* **🧠 Smart Resource Management:** Caches images in VRAM and preloads neighbors for a stutter-free browsing experience.

---

## 📖 Installation & Usage
Simply download the latest binary from the **[Releases page](https://github.com/icyhoty2k/PosMan/releases/latest)**.

**💡 Note:** If you move the executable to a new folder, run it once. The application will automatically detect the path change, self-heal the registry settings, and restore file associations.

---

## 🏗️ Architecture
Designed for developers who value imperative, explicit code over declarative abstractions:

* **`main.cpp`** : Application lifecycle and instance management.
* **`WicDecoder.cpp`** : Low-level WIC image decoding.
* **`RendererD2D/GDI.cpp`** : Hardware-accelerated (Direct2D) or fallback (GDI) rendering.
* **`AppState.h`** : Centralized, queryable system state.

---

## ⚙️ Build
Requires [CMake](https://cmake.org/) and an [MSVC](https://visualstudio.microsoft.com/) compiler.

```bash
git clone [https://github.com/icyhoty2k/QuickImageViewer.git](https://github.com/icyhoty2k/QuickImageViewer.git)
cd QuickImageViewer
mkdir build && cd build
cmake ..
cmake --build . --config Release
```
📜 License
Licensed under the GNU Affero General Public License v3.0 (AGPLv3). See the LICENSE file for details.
