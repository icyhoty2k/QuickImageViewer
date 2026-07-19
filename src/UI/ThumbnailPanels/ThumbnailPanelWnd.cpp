#include "ThumbnailPanelWnd.h"
#include <algorithm>
#include <filesystem>
#include <unordered_set>
#include <windowsx.h>
#include <d2d1.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>

#include "FileHandler.h"
#include "../UIManager.h"
#include "../../AppState.h"
#include "../../Platform/Constants.h"
#include "../../Input/Shortcuts.h"
#include "../../Input/AppCommands.h"
#include "../../Renderer/RendererD2D.h"
#include "../../Overlays/OverlayManager.h"
#include "../../Platform/ConstantsStrings.h"
#include "../ThemedDialog.h"

namespace UI {
    std::unordered_set<std::wstring> ThumbnailPanelWnd::s_cutPaths;
    std::wstring ThumbnailPanelWnd::s_dragSourcePath;

    std::wstring ThumbnailPanelWnd::FormatDirSize(int64_t bytes) {
        wchar_t buf[32];
        if (bytes >= 1024LL * 1024 * 1024)
            swprintf_s(buf, L"%.2f GB", bytes / (1024.0 * 1024 * 1024));
        else if (bytes >= 1024 * 1024)
            swprintf_s(buf, L"%.2f MB", bytes / (1024.0 * 1024));
        else if (bytes >= 1024)
            swprintf_s(buf, L"%.2f KB", bytes / 1024.0);
        else
            swprintf_s(buf, L"%lld B", bytes);
        return buf;
    }

    std::pair<std::wstring, UINT32> ThumbnailPanelWnd::BuildScrollbarLabel() const {
        const std::wstring folder = GetPanelFolder();
        if (folder.empty()) return {{}, 0};
        std::wstring text = folder;
        if (!m_dirSizeStr.empty()) text += L" → " + m_dirSizeStr;
        const auto lastSlash = folder.find_last_of(L"\\/");
        const UINT32 boldStart = (lastSlash != std::wstring::npos)
                                     ? static_cast<UINT32>(lastSlash + 1)
                                     : 0;
        return {std::move(text), boldStart};
    }

    void ThumbnailPanelWnd::ComputeDirSize() {
        if (GetPanelFolder().empty()) return;
        int64_t total = 0;
        std::error_code ec;
        // Primary DirWnd / CacheWnd items live in app.playlist, so their sizes
        // were already captured during the background scan into playlistFileSizes.
        // Prefer that (zero syscalls) and only hit the filesystem for items that
        // aren't in it (e.g. a SpawnedDirWnd showing a different folder).
        for (const auto &t: m_thumbnails) {
            auto it = app.playlistFileSizes.find(t.filePath);
            if (it != app.playlistFileSizes.end()) {
                total += it->second;
            } else {
                const auto sz = std::filesystem::file_size(t.filePath, ec);
                if (!ec) total += static_cast<int64_t>(sz);
                ec.clear();
            }
        }
        m_dirSizeBytes = total;
        m_dirSizeStr = FormatDirSize(total);
        m_dirLabelLayout.Reset(); // force label rebuild with new size
    }

    // Track the currently active panel window for border styling
    HWND g_activePanelHwnd = nullptr;

    void SetActivePanelWindow(HWND hWnd) {
        if (g_activePanelHwnd == hWnd) return; // Already active

        // Just track which window is active; opacity stays at 210
        HWND oldActive = g_activePanelHwnd;
        g_activePanelHwnd = hWnd;

        // Invalidate both old and new active windows so they redraw with updated border
        if (oldActive && IsWindow(oldActive)) {
            InvalidateRect(oldActive, nullptr, FALSE);
        }
        if (hWnd && IsWindow(hWnd)) {
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }

    // =========================================================================
    // OLE drag-and-drop helpers
    // =========================================================================

    // IDataObject carrying one or more CF_HDROP file paths.
    class PanelDataObject final : public IDataObject {
        std::vector<std::wstring> m_paths;
        ULONG m_ref = 1;

        public:
            explicit PanelDataObject(std::vector<std::wstring> paths) : m_paths(std::move(paths)) {}

            HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override {
                if (riid == IID_IUnknown || riid == IID_IDataObject) {
                    *ppv = this;
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG __stdcall AddRef() override {
                return InterlockedIncrement(&m_ref);
            }

            ULONG __stdcall Release() override {
                ULONG r = InterlockedDecrement(&m_ref);
                if (!r) delete this;
                return r;
            }

            HRESULT __stdcall GetData(FORMATETC *fmt, STGMEDIUM *med) override {
                if (!fmt || !med) return E_INVALIDARG;
                if (fmt->cfFormat != CF_HDROP || !(fmt->tymed & TYMED_HGLOBAL))
                    return DV_E_FORMATETC;
                // Build a double-null-terminated multi-path block.
                size_t totalChars = 1; // final null
                for (const auto &p: m_paths) totalChars += p.size() + 1;
                const size_t total = sizeof(DROPFILES) + totalChars * sizeof(wchar_t);
                HGLOBAL hg = GlobalAlloc(GHND, total);
                if (!hg) return E_OUTOFMEMORY;
                auto *df = static_cast<DROPFILES *>(GlobalLock(hg));
                df->pFiles = sizeof(DROPFILES);
                df->fWide = TRUE;
                df->pt = {};
                df->fNC = FALSE;
                wchar_t *dst = reinterpret_cast<wchar_t *>(df + 1);
                for (const auto &p: m_paths) {
                    wmemcpy(dst, p.c_str(), p.size() + 1);
                    dst += p.size() + 1;
                }
                *dst = L'\0';
                GlobalUnlock(hg);
                med->tymed = TYMED_HGLOBAL;
                med->hGlobal = hg;
                med->pUnkForRelease = nullptr;
                return S_OK;
            }

            HRESULT __stdcall QueryGetData(FORMATETC *fmt) override {
                if (!fmt) return E_INVALIDARG;
                return (fmt->cfFormat == CF_HDROP && (fmt->tymed & TYMED_HGLOBAL))
                           ? S_OK
                           : DV_E_FORMATETC;
            }

            HRESULT __stdcall GetDataHere(FORMATETC *, STGMEDIUM *) override {
                return E_NOTIMPL;
            }

            HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pOut) override {
                pOut->ptd = nullptr;
                return DATA_S_SAMEFORMATETC;
            }

            HRESULT __stdcall SetData(FORMATETC *, STGMEDIUM *, BOOL) override {
                return E_NOTIMPL;
            }

            HRESULT __stdcall EnumFormatEtc(DWORD, IEnumFORMATETC **) override {
                return E_NOTIMPL;
            }

            HRESULT __stdcall DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
                return E_NOTIMPL;
            }

            HRESULT __stdcall DUnadvise(DWORD) override {
                return E_NOTIMPL;
            }

            HRESULT __stdcall EnumDAdvise(IEnumSTATDATA **) override {
                return E_NOTIMPL;
            }
    };

    // IDropSource — uses OLE default cursors; Ctrl switches to copy.
    class PanelDropSource final : public IDropSource {
        ULONG m_ref = 1;

        public:
            HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override {
                if (riid == IID_IUnknown || riid == IID_IDropSource) {
                    *ppv = this;
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG __stdcall AddRef() override {
                return InterlockedIncrement(&m_ref);
            }

            ULONG __stdcall Release() override {
                ULONG r = InterlockedDecrement(&m_ref);
                if (!r) delete this;
                return r;
            }

            HRESULT __stdcall QueryContinueDrag(BOOL fEscPressed, DWORD grfKeyState) override {
                if (fEscPressed) return DRAGDROP_S_CANCEL;
                if (!(grfKeyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
                return S_OK;
            }

            HRESULT __stdcall GiveFeedback(DWORD) override {
                return DRAGDROP_S_USEDEFAULTCURSORS;
            }
    };

    // IDropTarget registered on each DirWnd panel window.
    // Default = move; Ctrl held = copy.
    class PanelDropTarget final : public IDropTarget {
        ThumbnailPanelWnd *m_panel;
        ULONG m_ref = 1;

        public:
            explicit PanelDropTarget(ThumbnailPanelWnd *panel) : m_panel(panel) {}

            HRESULT __stdcall QueryInterface(REFIID riid, void **ppv) override {
                if (riid == IID_IUnknown || riid == IID_IDropTarget) {
                    *ppv = this;
                    AddRef();
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }

            ULONG __stdcall AddRef() override {
                return InterlockedIncrement(&m_ref);
            }

            ULONG __stdcall Release() override {
                ULONG r = InterlockedDecrement(&m_ref);
                if (!r) delete this;
                return r;
            }

            static DWORD calcEffect(DWORD keyState) {
                return (keyState & MK_CONTROL) ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
            }

            HRESULT __stdcall DragEnter(IDataObject *pDataObj, DWORD keyState, POINTL, DWORD *pdwEffect) override {
                FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                if (FAILED(pDataObj->QueryGetData(&fe))) {
                    *pdwEffect = DROPEFFECT_NONE;
                    return S_OK;
                }
                *pdwEffect = calcEffect(keyState);
                return S_OK;
            }

            HRESULT __stdcall DragOver(DWORD keyState, POINTL, DWORD *pdwEffect) override {
                *pdwEffect = calcEffect(keyState);
                return S_OK;
            }

            HRESULT __stdcall DragLeave() override {
                return S_OK;
            }

            HRESULT __stdcall Drop(IDataObject *pDataObj, DWORD keyState, POINTL, DWORD *pdwEffect) override {
                const bool isCopy = (keyState & MK_CONTROL) != 0;
                *pdwEffect = isCopy ? DROPEFFECT_COPY : DROPEFFECT_MOVE;

                FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                STGMEDIUM stg = {};
                if (FAILED(pDataObj->GetData(&fe, &stg))) return S_OK;

                auto *hd = static_cast<HDROP>(GlobalLock(stg.hGlobal));
                if (!hd) {
                    ReleaseStgMedium(&stg);
                    return S_OK;
                }

                const UINT count = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
                std::wstring doubleNull;
                std::wstring firstSrcDir;
                for (UINT i = 0; i < count; ++i) {
                    const UINT len = DragQueryFileW(hd, i, nullptr, 0);
                    if (!len) continue;
                    std::wstring p(len + 1, L'\0');
                    DragQueryFileW(hd, i, p.data(), len + 1);
                    p.resize(len);
                    if (firstSrcDir.empty())
                        firstSrcDir = fs::path(p).parent_path().wstring();
                    doubleNull += p;
                    doubleNull += L'\0';
                }
                GlobalUnlock(stg.hGlobal);
                ReleaseStgMedium(&stg);

                if (doubleNull.empty()) return S_OK;
                doubleNull += L'\0';

                const std::wstring targetDir = m_panel->GetPanelFolder();
                if (targetDir.empty()) return S_OK;

                // Guard: don't move/copy within the same folder.
                if (!firstSrcDir.empty()) {
                    bool same = false;
                    try {
                        same = fs::equivalent(fs::path(firstSrcDir), fs::path(targetDir));
                    } catch (...) {
                        same = (firstSrcDir == targetDir);
                    }
                    if (same) return S_OK;
                }

                std::wstring to = targetDir + L'\0' + L'\0';
                SHFILEOPSTRUCTW op = {};
                op.hwnd = m_panel->GetOwnerHwnd();
                op.wFunc = isCopy ? FO_COPY : FO_MOVE;
                op.pFrom = doubleNull.c_str();
                op.pTo = to.c_str();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                SHFileOperationW(&op);

                uiManager.RefreshPanelDirs(firstSrcDir, targetDir);
                uiManager.RepaintAllPanels();
                return S_OK;
            }
    };

    // =========================================================================
    // Init
    // =========================================================================
    void ThumbnailPanelWnd::Init(HINSTANCE hInstance, HWND hParent) {
        Init(hInstance, hParent, 0);
    }

    void ThumbnailPanelWnd::Init(HINSTANCE hInstance, HWND hParent, int8_t position) {
        m_hOwner = hParent;
        m_position = position;

        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = IPanelWindow::WindowRouter;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = ClassName();
        RegisterClassW(&wc);

        int x, y, w, h;
        GetWindowBounds(hParent, m_position, x, y, w, h);

        CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                ClassName(),
                WindowTitle(),
                WS_POPUP,
                x, y, w, h,
                hParent, nullptr, hInstance,
                this // stored by WindowRouter on WM_NCCREATE
                );

        if (!m_hWnd) return;

        SetLayeredWindowAttributes(m_hWnd, 0, Constants::THUMBNAIL_PANEL_WINDOW_OPACITY, LWA_ALPHA);
        CreateDeviceResources();
        ShowWindow(m_hWnd, SW_HIDE);

        if (IsDirPanel())
            RegisterDragDrop(m_hWnd, new PanelDropTarget(this));
    }

    // =========================================================================
    // Toggle / Hide / MovePanel
    // =========================================================================
    void ThumbnailPanelWnd::Toggle() {
        if (!m_hWnd) return;
        if (IsWindowVisible(m_hWnd)) {
            Hide();
        } else {
            Show();
            SetFocus(m_hWnd);
            UpdateView();
            SyncSelectionRectangle();
        }
    }

    void ThumbnailPanelWnd::Hide() {
        if (m_hWnd && IsWindowVisible(m_hWnd)) {
            // Unbind: release all selection state and the position overlay slot.
            m_selectedPaths.clear();
            m_anchorPath.clear();
            g_overlayManager.UpdatePanelSelectionOverlay(m_position, 0, 0);
            if (m_hOwner) InvalidateRect(m_hOwner, nullptr, FALSE);
            ShowWindow(m_hWnd, SW_HIDE);
            uiManager.OnPanelHidden(this);
            // If this was the active dir panel, clear it so we fall back to F5.
            if (IsDirPanel()) uiManager.SetActiveDirWnd(nullptr);
        }
    }

    void ThumbnailPanelWnd::Show() {
        if (!m_hWnd) return;
        const PanelLayout &layout = uiManager.GetLayout();
        const SlotInfo *slot = layout.getSlot(m_position);
        if (slot && slot->panel != nullptr && slot->panel != this) {
            int8_t free = uiManager.NextFreePosition(m_position);
            if (free >= 0) m_position = free;
        }
        m_offset = 0.0f;
        // Bind: initialize selection state and claim the position overlay slot.
        m_selectedPaths.clear();
        m_anchorPath.clear();
        NotifySelectionOverlay();
        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetForegroundWindow(m_hWnd);
        uiManager.OnPanelShown(this, m_position);
    }

    void ThumbnailPanelWnd::MovePanel() {
        if (!m_hWnd) return;

        // Clear the selection overlay at the old position before moving.
        const int8_t oldPos = m_position;
        g_overlayManager.UpdatePanelSelectionOverlay(oldPos, 0, 0);

        // Remove from current slot.
        uiManager.OnPanelHidden(this);

        // Find next free position — skip slots occupied by other panels.
        int8_t next = uiManager.NextFreePosition(m_position);
        if (next < 0) return; // all slots occupied
        m_position = next;
        m_offset = 0.0f;

        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);

        // Register at new position — triggers RefreshVerticalPanels.
        uiManager.OnPanelShown(this, m_position);

        // Re-bind the selection overlay to the new position.
        NotifySelectionOverlay();
    }

    void ThumbnailPanelWnd::ClearDirThumbnailCache() {
        DoClearDirThumbnailCache();
        m_sourceDirty = true; // force PostBuildHook on next UpdateView to re-queue decodes
    }

    void ThumbnailPanelWnd::NotifySelectionOverlay() {
        const int sel = static_cast<int>(m_selectedPaths.size());
        const int total = static_cast<int>(m_thumbnails.size());
        g_overlayManager.UpdatePanelSelectionOverlay(m_position, sel, total);
        if (m_hOwner) InvalidateRect(m_hOwner, nullptr, FALSE);
    }

    void ThumbnailPanelWnd::RefreshBounds() {
        if (!m_hWnd) return;
        int x, y, w, h;
        GetWindowBounds(m_hOwner ? m_hOwner : m_hWnd, m_position, x, y, w, h);
        SetWindowPos(m_hWnd, HWND_TOPMOST, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }

    // =========================================================================
    // SyncSelectionRectangle
    // =========================================================================
    void ThumbnailPanelWnd::SyncSelectionRectangle() {
        if (!m_hWnd) return;

        m_selectedIdx = -1;

        // For panels with own playlist (F5, spawned), match by file path instead of index
        if (HasOwnPlaylist() && app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size())) {
            const std::wstring &currentPath = app.playlist[app.currentIndex];
            for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                if (m_thumbnails[i].filePath == currentPath) {
                    m_selectedIdx = static_cast<int>(i);
                    break;
                }
            }
        } else {
            // For shared-playlist panels, match by index
            for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                if (m_thumbnails[i].playlistIndex == app.currentIndex) {
                    m_selectedIdx = static_cast<int>(i);
                    break;
                }
            }
        }
        ScrollToSelected();
        // ScrollToSelected() may have changed m_offset. Recompute all thumbnail
        // rects so the render sees positions that match the new scroll position.
        // Without this, a large offset jump (e.g. image 800 → image 1) leaves
        // m_thumbnails with rects computed from the old offset, causing the
        // selection highlight to appear off-screen or at the wrong slot.
        RebuildGeometry();
        InvalidateRect(m_hWnd, nullptr, FALSE);
        UpdateWindow(m_hWnd);

        // Persist the final scroll+selection state so it can be restored
        // instantly when the user switches back to this panel.
        if (IsDirPanel() && m_selectedIdx >= 0 &&
            m_selectedIdx < static_cast<int>(m_thumbnails.size())) {
            m_savedSelectedIdx = m_selectedIdx;
            m_savedOffset = m_offset;
            m_savedImagePath = m_thumbnails[m_selectedIdx].filePath;
        }
    }

    // =========================================================================
    // UpdateView  —  rebuild geometry from source items + request async decodes
    // =========================================================================
    void ThumbnailPanelWnd::UpdateView() {
        if (!m_hWnd || !app.renderer || !IsWindowVisible(m_hWnd)) return;

        // Fast path for dir panels: skip membership rebuild + PostBuildHook when
        // the source playlist hasn't changed (e.g. pure scroll / image navigation).
        // CacheWnd is excluded (IsDirPanel() = false) so it always does a full rebuild.
        if (IsDirPanel() && !m_sourceDirty && !m_thumbnails.empty()) {
            RebuildGeometry();
            InvalidateRect(m_hWnd, nullptr, FALSE);
            return;
        }
        if (IsDirPanel()) m_sourceDirty = false;

        m_thumbnails.clear();

        // Spawned DirWnd instances own their own playlist and bypass this guard.
        // Primary DirWnd and CacheWnd still require app.playlist to be populated.
        if (!HasOwnPlaylist() && (app.playlist.empty() || app.currentIndex < 0)) {
            m_selectedPaths.clear();
            m_anchorPath.clear();
            NotifySelectionOverlay();
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return;
        }

        // Subclass provides the raw list; base owns all layout math.
        std::vector<std::wstring> items = GetSourceItems();
        if (items.empty()) {
            m_selectedPaths.clear();
            m_anchorPath.clear();
            NotifySelectionOverlay();
            m_emptyDirActive = true;
            m_dirSizeBytes = 0;
            m_dirSizeStr = FormatDirSize(0); // 0.00 B — folder exists but has no images
            m_dirLabelLayout.Reset();        // force label rebuild on next paint
            InvalidateRect(m_hWnd, nullptr, TRUE);
            return;
        }

        // Source content changed — prune the multi-selection to paths that still
        // exist instead of wiping it. CacheWnd rebuilds on every cache reorder
        // (each image view), so a full clear here made Ctrl/Shift+Click selections
        // vanish almost immediately on that panel. Selection is path-keyed, so it
        // is stable across the MRU reordering; only genuinely evicted/deleted
        // entries are dropped.
        if (!m_selectedPaths.empty() || !m_anchorPath.empty()) {
            const std::unordered_set<std::wstring> live(items.begin(), items.end());
            std::erase_if(m_selectedPaths,
                          [&](const std::wstring &p) {
                              return !live.count(p);
                          });
            if (!m_anchorPath.empty() && !live.count(m_anchorPath))
                m_anchorPath.clear();
            NotifySelectionOverlay();
        }
        m_emptyDirActive = false;
        m_emptyDirMissing = false;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);

        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
        float spacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        bool vertical = IsVertical();

        float x = margin;
        float y = margin;

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(items.size()) * (thumbW + spacing) - spacing;
            if (totalW <= surfaceW - 2.0f * margin) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOff = surfaceW - totalW - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                x = margin + m_offset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(items.size()) * (thumbH + spacing) - spacing;
            if (totalH <= surfaceH - 2.0f * margin) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOff = surfaceH - totalH - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                y = margin + m_offset;
            }
        }

        m_thumbnails.reserve(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            auto mapIt = app.playlistIndexMap.find(items[i]);

            // Assign -1 if the cached file is not in the current folder
            int idx = (mapIt != app.playlistIndexMap.end()) ? mapIt->second : -1;

            m_thumbnails.push_back({
                D2D1::RectF(x, y, x + thumbW, y + thumbH),
                items[i],
                idx
            });

            if (vertical) y += thumbH + spacing;
            else x += thumbW + spacing;
        }

        PostBuildHook();
        ComputeDirSize(); // uses m_thumbnails already built above; resets m_dirLabelLayout

        // Update m_selectedIdx to keep the highlight in sync with app.currentIndex,
        // but do NOT call SyncSelectionRectangle() / ScrollToSelected() here.
        // UpdateView() is called on every wheel tick and drag move; auto-scrolling
        // to the selected thumbnail on each call would prevent the user from
        // manually scrolling past it.  SyncSelectionRectangle() is called
        // explicitly by external navigation sites (AppMain, FileHandler) and by
        // WM_SIZE / Toggle() where a geometry rebuild warrants snapping to selection.
        m_selectedIdx = -1;
        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
            if (m_thumbnails[i].playlistIndex == app.currentIndex) {
                m_selectedIdx = static_cast<int>(i);
                break;
            }
        }

        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    // =========================================================================
    // RebuildGeometry
    // Recomputes every thumbnail's rect from the current m_offset without
    // touching m_thumbnails membership or calling PostBuildHook.
    // Called by SyncSelectionRectangle after ScrollToSelected moves m_offset
    // so the renderer draws geometry that matches the new scroll position.
    // =========================================================================
    void ThumbnailPanelWnd::RebuildGeometry() {
        if (m_thumbnails.empty()) return;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
        float spacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        bool vertical = IsVertical();

        float x = margin;
        float y = margin;

        size_t n = m_thumbnails.size();

        if (!vertical) {
            y = (surfaceH - thumbH) / 2.0f;
            float totalW = static_cast<float>(n) * (thumbW + spacing) - spacing;
            if (totalW <= surfaceW - 2.0f * margin) {
                x = (surfaceW - totalW) / 2.0f;
            } else {
                float minOff = surfaceW - totalW - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                x = margin + m_offset;
            }
        } else {
            x = (surfaceW - thumbW) / 2.0f;
            float totalH = static_cast<float>(n) * (thumbH + spacing) - spacing;
            if (totalH <= surfaceH - 2.0f * margin) {
                y = (surfaceH - totalH) / 2.0f;
            } else {
                float minOff = surfaceH - totalH - 2.0f * margin;
                m_offset = std::clamp(m_offset, minOff, 0.0f);
                y = margin + m_offset;
            }
        }

        // Re-derive the highlight while rebuilding rects. For panels that own
        // their playlist (F6, spawned) the cached playlistIndex values go stale
        // whenever app.playlist changes under them — e.g. the interim 1-file
        // playlist OpenSpecificImage installs while its background scan runs
        // (currentIndex == 0 then briefly matched a stale playlistIndex 0 and
        // flashed the selection on thumbnail 0 when switching between two
        // DirWnds). Match by file path for those panels — always authoritative.
        m_selectedIdx = -1;
        std::wstring curPath;
        if (app.currentIndex >= 0 && app.currentIndex < static_cast<int>(app.playlist.size()))
            curPath = app.playlist[app.currentIndex];
        const bool matchByPath = HasOwnPlaylist();
        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
            m_thumbnails[i].rect = D2D1::RectF(x, y, x + thumbW, y + thumbH);
            if (matchByPath
                    ? (!curPath.empty() && m_thumbnails[i].filePath == curPath)
                    : (app.currentIndex >= 0 && m_thumbnails[i].playlistIndex == app.currentIndex))
                m_selectedIdx = static_cast<int>(i);
            if (vertical) y += thumbH + spacing;
            else x += thumbW + spacing;
        }
    }

    // =========================================================================
    // ScrollToSelected
    // =========================================================================
    void ThumbnailPanelWnd::ScrollToSelected() {
        if (m_selectedIdx < 0 || m_selectedIdx >= static_cast<int>(m_thumbnails.size()))
            return;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        float surfaceW = static_cast<float>(cr.right);
        float surfaceH = static_cast<float>(cr.bottom);
        bool vertical = IsVertical();
        float thumbW = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
        float thumbH = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
        float scaledSpacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
        float scaledMargin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;

        if (!vertical) {
            float slotX = static_cast<float>(m_selectedIdx) * (thumbW + scaledSpacing);
            float visL = -m_offset + scaledMargin;
            float visR = visL + surfaceW - scaledMargin * 2.0f;
            if (slotX < visL) m_offset = -(slotX - scaledMargin);
            else if (slotX + thumbW > visR) m_offset = -(slotX + thumbW - surfaceW + scaledMargin);
        } else {
            float slotY = static_cast<float>(m_selectedIdx) * (thumbH + scaledSpacing);
            float visT = -m_offset + scaledMargin;
            float visB = visT + surfaceH - scaledMargin * 2.0f;
            if (slotY < visT) m_offset = -(slotY - scaledMargin);
            else if (slotY + thumbH > visB) m_offset = -(slotY + thumbH - surfaceH + scaledMargin);
        }
        InvalidateRect(m_hWnd, nullptr, FALSE);
    }

    // =========================================================================
    // IsVertical
    // Slot layout: 0=center-float  1=top  2=right  3=bottom  4=left
    // Vertical strips are right(2) and left(4).
    // =========================================================================
    bool ThumbnailPanelWnd::IsVertical() const {
        return (m_position == 2 || m_position == 4);
    }

    // =========================================================================
    // GetWindowBounds  —  shared 5-slot geometry for all thumbnail panels
    // =========================================================================
    void ThumbnailPanelWnd::GetWindowBounds(HWND hRef, int8_t position,
                                            int &x, int &y, int &w, int &h) const {
        HMONITOR hMonitor = MonitorFromWindow(hRef, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(hMonitor, &mi);

        // rcWork is the authoritative usable area — it already excludes the
        // taskbar on whichever edge it sits. Use it directly for all positions.
        int wx = mi.rcWork.left;
        int wy = mi.rcWork.top;
        int ww = mi.rcWork.right - mi.rcWork.left;
        int wh = mi.rcWork.bottom - mi.rcWork.top;

        // Shrink the work area by a small gap on every side that borders the
        // taskbar, so panels never appear glued to the taskbar edge.
        const int gap = Constants::THUMBNAIL_PANEL_TASKBAR_BOTTOM_GAP_HORIZONTAL_PANEL;
        if (mi.rcWork.left > mi.rcMonitor.left) {
            wx += gap;
            ww -= gap;
        }
        if (mi.rcWork.top > mi.rcMonitor.top) {
            wy += gap;
            wh -= gap;
        }
        if (mi.rcWork.right < mi.rcMonitor.right) {
            ww -= gap;
        }
        if (mi.rcWork.bottom < mi.rcMonitor.bottom) {
            wh -= gap;
        }

        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        float dpiScale = static_cast<float>(dpiX) / 96.0f;

        int horzThick = static_cast<int>(
            (Constants::THUMBNAIL_PANEL_THUMB_HEIGHT + Constants::THUMBNAIL_PANEL_THUMB_MARGIN * 2.0f) * dpiScale);
        int vertThick = static_cast<int>(
            (Constants::THUMBNAIL_PANEL_THUMB_WIDTH + Constants::THUMBNAIL_PANEL_THUMB_MARGIN * 2.0f) * dpiScale);

        // Vertical panels shrink to avoid overlapping visible horizontal panels.
        // Read directly from PanelLayout — always current, no polling needed.
        bool topOccupied = false;
        bool bottomOccupied = false;
        if (position == 2 || position == 4) {
            const PanelLayout &layout = uiManager.GetLayout();
            topOccupied = layout.topOccupied();
            bottomOccupied = layout.bottomOccupied();
        }

        int neighbourGap = Constants::THUMBNAIL_PANEL_NEIGHBOUR_GAP_VERTICAL_PANEL;
        auto verticalBounds = [&](int &vy, int &vh) {
            vy = wy + (topOccupied ? horzThick + neighbourGap : 0);
            int vBot = wy + wh - (bottomOccupied ? horzThick + neighbourGap : 0);
            vh = vBot - vy;
            if (vh < 0) vh = 0;
        };

        switch (position) {
            case 0: { // center floating
                int pw = static_cast<int>(ww * 0.80f);
                x = wx + (ww - pw) / 2;
                y = wy + (wh - horzThick) / 2;
                w = pw;
                h = horzThick;
                break;
            }
            case 1: // top
                x = wx;
                y = wy;
                w = ww;
                h = horzThick;
                break;
            case 2: { // right
                int vy, vh;
                verticalBounds(vy, vh);
                x = wx + ww - vertThick;
                y = vy;
                w = vertThick;
                h = vh;
                break;
            }
            case 3: // bottom
                x = wx;
                y = wy + wh - horzThick;
                w = ww;
                h = horzThick;
                break;
            case 4: { // left
                int vy, vh;
                verticalBounds(vy, vh);
                x = wx;
                y = vy;
                w = vertThick;
                h = vh;
                break;
            }
            default:
                x = wx;
                y = wy;
                w = ww;
                h = horzThick;
                break;
        }
    }

    // =========================================================================
    // HandleMessage  —  all shared Win32 logic
    // =========================================================================
    LRESULT ThumbnailPanelWnd::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_XBUTTONDOWN) {
            // Forward the event to the parent so MouseHandler can process it
            if (m_hOwner) {
                SendMessageW(m_hOwner, WM_XBUTTONDOWN, wParam, lParam);
            }
            return 0;
        }
        // File-scope drag state (per-instance via member vars)
        static POINT s_clickPos = {0, 0};
        static POINT s_lastMouse = {0, 0};
        static bool s_hasMoved = false;
        static bool s_dragging = false;

        // Handle focus events before the main switch
        if (message == WM_SETFOCUS) {
            SetActivePanelWindow(m_hWnd);
            return 0;
        }
        if (message == WM_KILLFOCUS) {
            // Invalidate to remove active border when focus is lost
            if (g_activePanelHwnd == m_hWnd) {
                InvalidateRect(m_hWnd, nullptr, FALSE);
            }
            return 0;
        }
        if (message == WM_MBUTTONUP) {
            Hide();
            return 0;
        }

        switch (message) {
            // -----------------------------------------------------------------
            case WM_PAINT: {
                PAINTSTRUCT ps;
                BeginPaint(m_hWnd, &ps);
                Render(m_selectedIdx, m_hoverIdx);
                EndPaint(m_hWnd, &ps);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SIZE: {
                ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
                UpdateView();
                SyncSelectionRectangle(); // geometry changed — snap to current image
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
                return 0;

            // -----------------------------------------------------------------
            case WM_MOUSEWHEEL: {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                float scroll = Constants::THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_SPEED;
                if (GetKeyState(VK_SHIFT) & 0x8000) scroll *= 3.0f;
                const float step = (delta > 0 ? scroll : -scroll)
                                   * Constants::THUMBNAIL_PANEL_WINDOW_MOUSE_WHEEL_DIRECTION;

                // Wrap-around: when already at a boundary and the wheel pushes
                // past it, jump to the opposite end instead of doing nothing.
                // m_offset range is [minOff, 0]: 0 = start, minOff (<0) = end.
                {
                    RECT crw{};
                    GetClientRect(m_hWnd, &crw);
                    const bool vert = IsVertical();
                    const float thumbSz = (vert
                                               ? Constants::THUMBNAIL_PANEL_THUMB_HEIGHT
                                               : Constants::THUMBNAIL_PANEL_THUMB_WIDTH) * app.dpiScale;
                    const float spacing = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                    const float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
                    const float surface = static_cast<float>(vert ? crw.bottom : crw.right);
                    const float total = static_cast<float>(m_thumbnails.size()) * (thumbSz + spacing) - spacing;
                    const float minOff = surface - total - 2.0f * margin;

                    if (minOff < 0.0f) { // content overflows — scrolling is possible
                        const float eps = 0.5f;
                        if (app.thumbnailPanelWheelWrapAround) {
                            if (step < 0.0f && m_offset <= minOff + eps) {
                                m_offset = 0.0f; // at end, pushing further → wrap to start
                                if constexpr (Constants::THUMBNAIL_PANEL_WHEEL_WRAP_OVERLAY)
                                    g_overlayManager.PostCenterMessage(m_hOwner,
                                                                       Constants::Messages::THUMB_STRIP_WRAP_TO_START);
                            } else if (step > 0.0f && m_offset >= -eps) {
                                m_offset = minOff; // at start, pushing back → wrap to end
                                if constexpr (Constants::THUMBNAIL_PANEL_WHEEL_WRAP_OVERLAY)
                                    g_overlayManager.PostCenterMessage(m_hOwner,
                                                                       Constants::Messages::THUMB_STRIP_WRAP_TO_END);
                            } else {
                                m_offset += step; // normal scroll (clamped in rebuild)
                            }
                        } else {
                            m_offset += step; // wrap disabled — clamp in rebuild
                        }
                    } else {
                        m_offset += step;
                    }
                }
                UpdateView();
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_KEYDOWN: {
                int key = static_cast<int>(wParam);

                if (key == GetKeyToggle()) {
                    uiManager.Toggle(*this);
                    return 0;
                }
                if (key == GetKeyMove()) {
                    MovePanel();
                    return 0;
                }
                if (GetKeyExtra() != -1 && key == GetKeyExtra()) {
                    OnExtraKey();
                    return 0;
                }

                // Panel-level file operations — act on multi-selection if active,
                // otherwise act on the currently hovered / single item.
                if (IsDirPanel()) {
                    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                    // Build operation set: selection if non-empty, else hovered item.
                    auto buildOpPaths = [&]() -> std::vector<std::wstring> {
                        if (!m_selectedPaths.empty())
                            return std::vector<std::wstring>(m_selectedPaths.begin(), m_selectedPaths.end());
                        if (m_hoverIdx >= 0 && m_hoverIdx < static_cast<int>(m_thumbnails.size()))
                            return {m_thumbnails[m_hoverIdx].filePath};
                        return {};
                    };

                    if (ctrl && key == 'C') {
                        const auto paths = buildOpPaths();
                        if (!paths.empty()) {
                            s_cutPaths.clear();
                            AppCommands::CopyFilesToClipboard(m_hOwner, paths);
                            uiManager.RepaintAllPanels();
                        }
                        return 0;
                    }
                    if (ctrl && key == 'X') {
                        const auto paths = buildOpPaths();
                        if (!paths.empty()) {
                            AppCommands::CopyFilesToClipboard(m_hOwner, paths, true);
                            s_cutPaths.clear();
                            for (const auto &p: paths) s_cutPaths.insert(p);
                            uiManager.RepaintAllPanels();
                        }
                        return 0;
                    }
                    if (ctrl && key == 'V') {
                        std::wstring pasteDir = GetPanelFolder();
                        if (!pasteDir.empty() && AppCommands::ClipboardHasFiles()) {
                            std::wstring srcDir;
                            if (!s_cutPaths.empty())
                                srcDir = fs::path(*s_cutPaths.begin()).parent_path().wstring();
                            AppCommands::PasteFilesFromClipboard(m_hOwner, pasteDir);
                            s_cutPaths.clear();
                            uiManager.RefreshPanelDirs(srcDir, pasteDir);
                            uiManager.RepaintAllPanels();
                        }
                        return 0;
                    }
                    if (ctrl && key == 'A') {
                        if (m_selectedPaths.size() == m_thumbnails.size())
                            m_selectedPaths.clear();
                        else
                            for (const auto &t: m_thumbnails) m_selectedPaths.insert(t.filePath);
                        m_anchorPath = (!m_selectedPaths.empty() && !m_thumbnails.empty())
                                           ? m_thumbnails[0].filePath
                                           : L"";
                        NotifySelectionOverlay();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        return 0;
                    }
                    if (key == VK_DELETE) {
                        const auto paths = buildOpPaths();
                        if (!paths.empty()) {
                            for (const auto &p: paths) s_cutPaths.erase(p);
                            OnContextMenuDelete(paths);
                            m_selectedPaths.clear();
                            m_anchorPath.clear();
                            NotifySelectionOverlay();
                        }
                        return 0;
                    }
                    if (key == VK_ESCAPE && !m_selectedPaths.empty()) {
                        m_selectedPaths.clear();
                        m_anchorPath.clear();
                        NotifySelectionOverlay();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        return 0;
                    }
                }

                // Forward unhandled keys to the main window
                if (m_hOwner) return SendMessageW(m_hOwner, message, wParam, lParam);
                break;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONDOWN: {
                m_justActivated = IsDirPanel() && (&uiManager.getActiveDirWnd() != this);
                if (IsDirPanel()) uiManager.SetActiveDirWnd(this);
                // Silently pre-restore saved scroll + selection so that:
                //  (a) empty-space clicks reload the saved image without a jump, and
                //  (b) thumbnail clicks produce a movement from the saved position to the
                //      new one rather than from some stale/wrong position.
                // We deliberately skip RebuildGeometry and InvalidateRect here:
                // m_thumbnails[i].rect stays based on the current visible offset so the
                // hit test in WM_LBUTTONUP still maps click coordinates correctly.
                // SyncSelectionRectangle (fired by the load) calls RebuildGeometry and
                // triggers the first paint once the new image is ready.
                if (m_justActivated && m_savedSelectedIdx >= 0 &&
                    m_savedSelectedIdx < static_cast<int>(m_thumbnails.size()) &&
                    m_thumbnails[m_savedSelectedIdx].filePath == m_savedImagePath) {
                    m_selectedIdx = m_savedSelectedIdx;
                    m_offset = m_savedOffset;
                }

                // Check if the click landed on the scrollbar strip.
                {
                    const int cx = GET_X_LPARAM(lParam);
                    const int cy = GET_Y_LPARAM(lParam);
                    RECT cr2{};
                    GetClientRect(m_hWnd, &cr2);
                    const float sw2 = static_cast<float>(cr2.right);
                    const float sh2 = static_cast<float>(cr2.bottom);
                    const float bar = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                    const bool vert = IsVertical();

                    bool inScrollbar = false;
                    if (vert) {
                        // Vertical panels: check x position based on m_position
                        if (m_position == 2) {
                            // Right panel: scrollbar on LEFT if SCROLLBAR_POS_RIGHT_PANEL == LEFT
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL
                                           == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                              ? (static_cast<float>(cx) < bar)
                                              : (static_cast<float>(cx) >= sw2 - bar);
                        } else {
                            // Left panel: scrollbar on RIGHT if SCROLLBAR_POS_LEFT_PANEL == RIGHT
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL
                                           == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                              ? (static_cast<float>(cx) < bar)
                                              : (static_cast<float>(cx) >= sw2 - bar);
                        }
                    } else {
                        // Horizontal panels: check y position based on m_position
                        if (m_position == 1) {
                            // Top panel: scrollbar at TOP or BOTTOM
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL
                                           == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                              ? (static_cast<float>(cy) < bar)
                                              : (static_cast<float>(cy) >= sh2 - bar);
                        } else {
                            // Bottom panel: scrollbar at TOP or BOTTOM
                            inScrollbar = (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL
                                           == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                              ? (static_cast<float>(cy) < bar)
                                              : (static_cast<float>(cy) >= sh2 - bar);
                        }
                    }

                    if (inScrollbar && !m_thumbnails.empty()) {
                        const float tw2 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
                        const float th2 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                        const float sp2 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                        const float mg2 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
                        const float n2 = static_cast<float>(m_thumbnails.size());
                        const float tsz = vert ? th2 : tw2;
                        const float content2 = n2 * (tsz + sp2) - sp2;
                        const float visible2 = (vert ? sh2 : sw2) - 2.0f * mg2;

                        if (content2 > visible2) {
                            m_scrollDragging = true;
                            m_scrollDragStartMouse = vert
                                                         ? static_cast<float>(cy)
                                                         : static_cast<float>(cx);
                            m_scrollDragStartOffset = m_offset;
                            SetCapture(m_hWnd);
                            return 0;
                        }
                    }
                }

                // Record which thumbnail the LMB landed on (for OLE drag).
                if (IsDirPanel()) {
                    s_dragSourcePath.clear();
                    const int hx = GET_X_LPARAM(lParam);
                    const int hy = GET_Y_LPARAM(lParam);
                    for (const auto &t: m_thumbnails) {
                        if (t.HitTest(hx, hy)) {
                            s_dragSourcePath = t.filePath;
                            break;
                        }
                    }
                }

                s_clickPos.x = GET_X_LPARAM(lParam);
                s_clickPos.y = GET_Y_LPARAM(lParam);
                s_hasMoved = false;
                s_dragging = true;
                SetCapture(m_hWnd);
                GetCursorPos(&s_lastMouse);
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);

                // Check if hovering over scrollbar strip
                RECT cr3{};
                GetClientRect(m_hWnd, &cr3);
                const float sw3 = static_cast<float>(cr3.right);
                const float sh3 = static_cast<float>(cr3.bottom);
                const float bar3 = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                const bool vert3 = IsVertical();

                bool inScrollbar3 = false;
                if (vert3) {
                    if (m_position == 2) {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL
                                        == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                           ? (static_cast<float>(x) < bar3)
                                           : (static_cast<float>(x) >= sw3 - bar3);
                    } else {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL
                                        == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                           ? (static_cast<float>(x) < bar3)
                                           : (static_cast<float>(x) >= sw3 - bar3);
                    }
                } else {
                    if (m_position == 1) {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL
                                        == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                           ? (static_cast<float>(y) < bar3)
                                           : (static_cast<float>(y) >= sh3 - bar3);
                    } else {
                        inScrollbar3 = (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL
                                        == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                           ? (static_cast<float>(y) < bar3)
                                           : (static_cast<float>(y) >= sh3 - bar3);
                    }
                }

                if (!m_thumbnails.empty() && inScrollbar3) {
                    const float tw3 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
                    const float th3 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                    const float sp3 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                    const float mg3 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
                    const float n3 = static_cast<float>(m_thumbnails.size());
                    const float tsz3 = vert3 ? th3 : tw3;
                    const float content3 = n3 * (tsz3 + sp3) - sp3;
                    const float visible3 = (vert3 ? sh3 : sw3) - 2.0f * mg3;

                    if (content3 > visible3) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                    } else {
                        SetCursor(LoadCursor(nullptr, IDC_ARROW));
                    }
                } else {
                    SetCursor(LoadCursor(nullptr, IDC_ARROW));
                }

                int newHover = -1;
                for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                    if (m_thumbnails[i].HitTest(x, y)) {
                        newHover = static_cast<int>(i);
                        break;
                    }
                }
                if (newHover != m_hoverIdx) {
                    m_hoverIdx = newHover;
                    InvalidateRect(m_hWnd, nullptr, FALSE);
                }

                if (m_scrollDragging) {
                    RECT cr2{};
                    GetClientRect(m_hWnd, &cr2);
                    const float sw2 = static_cast<float>(cr2.right);
                    const float sh2 = static_cast<float>(cr2.bottom);
                    const bool vert = IsVertical();
                    const float mouseAxis = vert ? static_cast<float>(y) : static_cast<float>(x);
                    const float tw2 = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
                    const float th2 = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                    const float sp2 = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
                    const float mg2 = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
                    const float n2 = static_cast<float>(m_thumbnails.size());
                    const float tsz = vert ? th2 : tw2;
                    const float content2 = n2 * (tsz + sp2) - sp2;
                    const float visible2 = (vert ? sh2 : sw2) - 2.0f * mg2;
                    const float scrollMax = content2 - visible2;
                    const float trackLen = vert ? sh2 : sw2;
                    const float barThumb = std::max(Constants::ThumbnailPanel::SCROLLBAR_MIN_THUMB * app.dpiScale,
                                                    trackLen * visible2 / content2);
                    const float scrollPx = trackLen - barThumb;
                    if (scrollPx > 0.0f) {
                        const float delta = mouseAxis - m_scrollDragStartMouse;
                        const float newScroll = std::clamp(
                                -m_scrollDragStartOffset + delta * scrollMax / scrollPx,
                                0.0f, scrollMax);
                        m_offset = -newScroll;
                        RebuildGeometry();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                    }
                    return 0;
                }

                if (s_dragging) {
                    const int dx = abs(x - s_clickPos.x);
                    const int dy = abs(y - s_clickPos.y);
                    if (dx > 5 || dy > 5) s_hasMoved = true;

                    // Initiate OLE drag-and-drop when mouse exceeds the system drag threshold.
                    if (IsDirPanel() && !s_dragSourcePath.empty() &&
                        (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG))) {
                        // If the item under the cursor is part of the multi-selection,
                        // drag the entire selection; otherwise drag only that item.
                        std::vector<std::wstring> dragPaths;
                        if (!m_selectedPaths.empty() && m_selectedPaths.count(s_dragSourcePath)) {
                            dragPaths.assign(m_selectedPaths.begin(), m_selectedPaths.end());
                        } else {
                            dragPaths = {std::move(s_dragSourcePath)};
                        }
                        s_dragSourcePath.clear();
                        s_dragging = false;
                        ReleaseCapture();
                        auto *pData = new PanelDataObject(std::move(dragPaths));
                        auto *pSrc = new PanelDropSource();
                        DWORD dwEffect = 0;
                        DoDragDrop(pData, pSrc, DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);
                        pData->Release();
                        pSrc->Release();
                        return 0;
                    }

                    POINT cur;
                    GetCursorPos(&cur);
                    float delta = IsVertical()
                                      ? static_cast<float>(cur.y - s_lastMouse.y)
                                      : static_cast<float>(cur.x - s_lastMouse.x);
                    m_offset += delta;
                    s_lastMouse = cur;
                    UpdateView();
                }
                return 0;
            }

            // -----------------------------------------------------------------
            case WM_LBUTTONUP: {
                s_dragSourcePath.clear();
                if (m_scrollDragging) {
                    m_scrollDragging = false;
                    ReleaseCapture();
                    return 0;
                }
                if (s_dragging) {
                    ReleaseCapture();
                    if (!s_hasMoved) {
                        // Empty-dir placeholder click → open folder in Explorer.
                        if (m_emptyDirActive && m_thumbnails.empty() && !m_emptyDir.empty()) {
                            ShellExecuteW(nullptr, L"explore", m_emptyDir.c_str(),
                                          nullptr, nullptr, SW_SHOWNORMAL);
                            s_dragging = false;
                            return 0;
                        }
                        const int cx = GET_X_LPARAM(lParam);
                        const int cy = GET_Y_LPARAM(lParam);
                        const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                        const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                        int hitIdx = -1;
                        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
                            if (m_thumbnails[i].HitTest(cx, cy)) {
                                hitIdx = static_cast<int>(i);
                                break;
                            }
                        }

                        if (hitIdx < 0) {
                            if (m_justActivated && !m_savedImagePath.empty()) {
                                // Reload the image last viewed in this panel.
                                // m_offset and m_selectedIdx were already pre-set in
                                // WM_LBUTTONDOWN, so SyncSelectionRectangle will find the
                                // saved item in view and the rectangle will not jump.
                                auto restoreIt = app.playlistIndexMap.find(m_savedImagePath);
                                if (restoreIt != app.playlistIndexMap.end())
                                    LoadImageIndex(m_hOwner, restoreIt->second);
                                else
                                    OpenSpecificImage(m_hOwner, m_savedImagePath);
                            } else if (!m_selectedPaths.empty()) {
                                // Plain empty-space click — clear multi-selection
                                // (Explorer behavior).
                                m_selectedPaths.clear();
                                m_anchorPath.clear();
                                NotifySelectionOverlay();
                                InvalidateRect(m_hWnd, nullptr, FALSE);
                            }
                            m_justActivated = false;
                        } else {
                            m_justActivated = false;
                            const std::wstring &hitPath = m_thumbnails[hitIdx].filePath;

                            if (ctrlDown && !shiftDown) {
                                // Ctrl+Click: toggle item in multi-selection; update anchor.
                                if (m_selectedPaths.count(hitPath))
                                    m_selectedPaths.erase(hitPath);
                                else
                                    m_selectedPaths.insert(hitPath);
                                m_anchorPath = hitPath;
                                NotifySelectionOverlay();
                                InvalidateRect(m_hWnd, nullptr, FALSE);
                            } else if (shiftDown) {
                                // Shift+Click: REPLACE selection with anchor→click range
                                // (Explorer semantics). Ctrl+Shift+Click ADDS the range
                                // to the existing selection instead.
                                // Must NEVER fall through to the plain-click branch —
                                // a Shift+Click that activated the image (because no
                                // anchor existed yet) read as a stray left click.
                                int anchorIdx = -1;
                                if (!m_anchorPath.empty()) {
                                    for (size_t k = 0; k < m_thumbnails.size(); ++k) {
                                        if (m_thumbnails[k].filePath == m_anchorPath) {
                                            anchorIdx = static_cast<int>(k);
                                            break;
                                        }
                                    }
                                }
                                if (!ctrlDown) m_selectedPaths.clear();
                                if (anchorIdx >= 0) {
                                    const int lo = std::min(anchorIdx, hitIdx);
                                    const int hi = std::max(anchorIdx, hitIdx);
                                    for (int k = lo; k <= hi; ++k)
                                        m_selectedPaths.insert(m_thumbnails[k].filePath);
                                    // Anchor stays put so a subsequent Shift+Click
                                    // re-ranges from the same origin (Explorer behavior).
                                } else {
                                    // First Shift+Click with no anchor: mark just this
                                    // item and make it the range start — the next
                                    // Shift+Click selects from here.
                                    m_selectedPaths.insert(hitPath);
                                    m_anchorPath = hitPath;
                                }
                                NotifySelectionOverlay();
                                InvalidateRect(m_hWnd, nullptr, FALSE);
                            } else {
                                // Plain click: clear selection, open image, set anchor.
                                m_selectedPaths.clear();
                                m_anchorPath = hitPath;
                                NotifySelectionOverlay();
                                // Re-check the live index map at click time — the cached
                                // playlistIndex can be stale if the folder changed since
                                // the last UpdateView (e.g. clicking between spawned panels).
                                auto mapIt = app.playlistIndexMap.find(hitPath);
                                if (mapIt != app.playlistIndexMap.end()) {
                                    LoadImageIndex(m_hOwner, mapIt->second);
                                } else {
                                    OpenSpecificImage(m_hOwner, hitPath);
                                }
                                UpdateView();
                            }
                        }
                    }
                    s_dragging = false;
                }
                return 0;
            }
            // -----------------------------------------------------------------
            case WM_RBUTTONUP: {
                const int rx = GET_X_LPARAM(lParam);
                const int ry = GET_Y_LPARAM(lParam);

                // Identify which thumbnail was right-clicked.
                std::wstring hitPath;
                for (const auto &t: m_thumbnails) {
                    if (t.HitTest(rx, ry)) {
                        hitPath = t.filePath;
                        break;
                    }
                }

                // Determine the effective operation set:
                // - hit item is in the multi-selection → operate on the whole selection
                // - hit item not in selection (or no selection) → operate on hit item only
                // - right-click on empty space with selection active → operate on selection
                std::vector<std::wstring> opPaths;
                const bool hitInSel = !hitPath.empty() && m_selectedPaths.count(hitPath) > 0;
                if (!m_selectedPaths.empty() && (hitPath.empty() || hitInSel)) {
                    opPaths.assign(m_selectedPaths.begin(), m_selectedPaths.end());
                } else if (!hitPath.empty()) {
                    opPaths = {hitPath};
                }

                // Paste target: prefer the hit item's folder, then any visible
                // thumbnail's folder, then the empty-dir path shown as placeholder.
                std::wstring pasteDir;
                if (!hitPath.empty())
                    pasteDir = fs::path(hitPath).parent_path().wstring();
                else if (!m_thumbnails.empty())
                    pasteDir = fs::path(m_thumbnails[0].filePath).parent_path().wstring();
                else if (!m_emptyDir.empty())
                    pasteDir = m_emptyDir;

                constexpr UINT ID_CTX_COPY = 1;
                constexpr UINT ID_CTX_CUT = 2;
                constexpr UINT ID_CTX_DELETE = 3;
                constexpr UINT ID_CTX_PASTE = 4;
                constexpr UINT ID_CTX_EXTRA = 5;
                constexpr UINT ID_CTX_CLOSE = 6;
                constexpr UINT ID_CTX_SELECT_ALL = 7;
                constexpr UINT ID_CTX_SELECT_NONE = 8;
                constexpr UINT ID_CTX_SELECT_INVERSE = 9;

                const bool hasFiles = !opPaths.empty();
                const bool canPaste = !pasteDir.empty() && AppCommands::ClipboardHasFiles();
                const bool showPaste = ShowContextMenuPaste();
                const wchar_t *delLabel = ContextMenuDeleteLabel();
                const wchar_t *extraLabel = ContextMenuExtraLabel();

                // Build labels that show the count when multiple files are selected.
                std::wstring copyLabel = L"Copy";
                std::wstring cutLabel = L"Cut";
                std::wstring deleteLabel = delLabel;
                if (opPaths.size() > 1) {
                    const std::wstring cnt = L" (" + std::to_wstring(opPaths.size()) + L" files)";
                    copyLabel += cnt;
                    cutLabel += cnt;
                    deleteLabel += cnt;
                }

                const bool hasAny = !m_thumbnails.empty();

                HMENU hMenu = CreatePopupMenu();
                if (!hMenu) return 0;
                AppendMenuW(hMenu, hasFiles ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_COPY, copyLabel.c_str());
                AppendMenuW(hMenu, hasFiles ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_CUT, cutLabel.c_str());
                AppendMenuW(hMenu, hasFiles ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_DELETE, deleteLabel.c_str());
                if (showPaste) {
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, canPaste ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_PASTE, L"Paste");
                }
                if (extraLabel) {
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, ID_CTX_EXTRA, extraLabel);
                }
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, hasAny ? MF_STRING : (MF_STRING | MF_GRAYED), ID_CTX_SELECT_ALL, L"Select All");
                AppendMenuW(hMenu, hasAny ? MF_STRING : (MF_STRING | MF_GRAYED),
                            ID_CTX_SELECT_INVERSE, L"Select Inverse");
                AppendMenuW(hMenu, !m_selectedPaths.empty() ? MF_STRING : (MF_STRING | MF_GRAYED),
                            ID_CTX_SELECT_NONE, L"Select None");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, ID_CTX_CLOSE, L"Close");

                POINT pt = {rx, ry};
                ClientToScreen(m_hWnd, &pt);
                const UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, 0, m_hWnd, nullptr);
                DestroyMenu(hMenu);

                switch (cmd) {
                    case ID_CTX_COPY:
                        s_cutPaths.clear();
                        AppCommands::CopyFilesToClipboard(m_hOwner, opPaths);
                        uiManager.RepaintAllPanels();
                        break;
                    case ID_CTX_CUT:
                        AppCommands::CopyFilesToClipboard(m_hOwner, opPaths, true);
                        s_cutPaths.clear();
                        for (const auto &p: opPaths) s_cutPaths.insert(p);
                        uiManager.RepaintAllPanels();
                        break;
                    case ID_CTX_DELETE:
                        for (const auto &p: opPaths) s_cutPaths.erase(p);
                        OnContextMenuDelete(opPaths);
                        m_selectedPaths.clear();
                        m_anchorPath.clear();
                        NotifySelectionOverlay();
                        break;
                    case ID_CTX_PASTE: {
                        std::wstring srcDir;
                        if (!s_cutPaths.empty())
                            srcDir = fs::path(*s_cutPaths.begin()).parent_path().wstring();
                        AppCommands::PasteFilesFromClipboard(m_hOwner, pasteDir);
                        s_cutPaths.clear();
                        uiManager.RefreshPanelDirs(srcDir, pasteDir);
                        uiManager.RepaintAllPanels();
                        break;
                    }
                    case ID_CTX_EXTRA:
                        OnContextMenuExtra();
                        break;
                    case ID_CTX_SELECT_ALL:
                        for (const auto &t: m_thumbnails) m_selectedPaths.insert(t.filePath);
                        if (!m_thumbnails.empty()) m_anchorPath = m_thumbnails[0].filePath;
                        NotifySelectionOverlay();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        break;
                    case ID_CTX_SELECT_INVERSE: {
                        // Selected → unselected, unselected → selected.
                        std::unordered_set<std::wstring> inverted;
                        for (const auto &t: m_thumbnails)
                            if (!m_selectedPaths.count(t.filePath))
                                inverted.insert(t.filePath);
                        m_selectedPaths = std::move(inverted);
                        // Keep a valid anchor: first selected item in strip order,
                        // or cleared when the inversion selected nothing.
                        m_anchorPath.clear();
                        for (const auto &t: m_thumbnails)
                            if (m_selectedPaths.count(t.filePath)) {
                                m_anchorPath = t.filePath;
                                break;
                            }
                        NotifySelectionOverlay();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        break;
                    }
                    case ID_CTX_SELECT_NONE:
                        m_selectedPaths.clear();
                        m_anchorPath.clear();
                        NotifySelectionOverlay();
                        InvalidateRect(m_hWnd, nullptr, FALSE);
                        break;
                    case ID_CTX_CLOSE:
                        Hide();
                        break;
                }
                return 0;
            }

            case Constants::WM_QIV_REPAINT:
                InvalidateRect(m_hWnd, nullptr, FALSE);
                return 0;
            // -----------------------------------------------------------------
            case WM_CLOSE:
                ShowWindow(m_hWnd, SW_HIDE);
                return 0;
            // -----------------------------------------------------------------
            case WM_DPICHANGED: {
                // Thumbnail cache holds bitmaps decoded at the old physical resolution.
                // Clear them so new requests use the updated app.dpiScale.
                ClearDirThumbnailCache();
                // RefreshBounds recomputes physical panel size at the new DPI and calls
                // SetWindowPos, which generates WM_SIZE → ResizeSwapChain + UpdateView.
                RefreshBounds();
                return 0;
            }
            // -----------------------------------------------------------------
            case WM_DESTROY:
                if (IsDirPanel()) RevokeDragDrop(m_hWnd);
                return 0;
        }

        return DefWindowProcW(m_hWnd, message, wParam, lParam);
    }

    // =========================================================================
    // RenderEmptyPlaceholder
    void ThumbnailPanelWnd::OnContextMenuDelete(const std::vector<std::wstring> &paths) {
        if (paths.empty()) return;

        std::wstring msg;
        if (paths.size() == 1) {
            const std::wstring &name = paths[0].substr(paths[0].find_last_of(L"\\/") + 1);
            msg = L"Move to Recycle Bin?\n\n" + name;
        } else {
            msg = L"Move " + std::to_wstring(paths.size()) + L" files to Recycle Bin?";
            constexpr size_t MAX_LISTED = 5;
            for (size_t i = 0; i < std::min(paths.size(), MAX_LISTED); ++i)
                msg += L"\n  " + paths[i].substr(paths[i].find_last_of(L"\\/") + 1);
            if (paths.size() > MAX_LISTED)
                msg += L"\n  … and " + std::to_wstring(paths.size() - MAX_LISTED) + L" more";
        }

        if (!ThemedDialog::Confirm(m_hOwner, msg.c_str(), L"Delete")) return;

        AppCommands::DeleteFilesToRecycleBin(paths);
    }

    // Drawn when the scanned folder has no images. Fills the full panel area
    // with a centered two-line label: "No Images" + the folder path (wrapping
    // at panel edges). A click anywhere opens the folder in Explorer.
    // =========================================================================
    void ThumbnailPanelWnd::RenderEmptyPlaceholder() {
        if (!m_panelContext || !app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (!r || !r->m_pDWriteFactory || !r->m_pTextBrush) return;

        RECT cr{};
        GetClientRect(m_hWnd, &cr);
        const float sw = static_cast<float>(cr.right);
        const float sh = static_cast<float>(cr.bottom);
        const float margin = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
        const float textW = std::max(1.0f, sw - 2.0f * margin);

        // Lazily create a DWrite format for the panel's own use.
        // m_pTextFormat on the renderer is never initialized; own it here instead.
        if (!m_emptyFormat) {
            const float fontSize = std::max(8.0f, 11.0f * app.dpiScale);
            r->m_pDWriteFactory->CreateTextFormat(
                    L"Segoe UI", nullptr,
                    DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, fontSize,
                    Constants::Overlay::MSG_ALL_FONT_LOCALE,
                    m_emptyFormat.GetAddressOf());
        }
        if (!m_emptyFormat) return;

        // CacheWnd: not a dir panel — show a single centered "Thumbnail Cache Empty" line.
        if (!IsDirPanel()) {
            if (!m_emptyCacheLayout) {
                const wchar_t *txt = Constants::Messages::EMPTY_CACHE;
                r->m_pDWriteFactory->CreateTextLayout(
                        txt, static_cast<UINT32>(wcslen(txt)),
                        m_emptyFormat.Get(), std::max(1.0f, sw - 2.0f * margin), sh,
                        &m_emptyCacheLayout);
                if (m_emptyCacheLayout) {
                    m_emptyCacheLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_emptyCacheLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
                }
            }
            if (m_emptyCacheLayout) {
                DWRITE_TEXT_METRICS m{};
                m_emptyCacheLayout->GetMetrics(&m);
                m_panelContext->DrawTextLayout(
                        {margin, (sh - m.height) / 2.0f},
                        m_emptyCacheLayout.Get(),
                        r->m_pTextBrush.Get());
            }
            return;
        }

        // Choose header text and brush based on missing vs empty-but-present state.
        ID2D1SolidColorBrush *headerBrush;
        IDWriteTextLayout *headerLayout;

        if (m_emptyDirMissing) {
            // Directory itself is gone — red warning.
            if (!m_emptyDirMissingLayout) {
                const wchar_t *txt = Constants::Messages::EMPTY_DIR_MISSING;
                r->m_pDWriteFactory->CreateTextLayout(
                        txt, static_cast<UINT32>(wcslen(txt)),
                        m_emptyFormat.Get(), textW, sh,
                        &m_emptyDirMissingLayout);
                if (m_emptyDirMissingLayout) {
                    m_emptyDirMissingLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_emptyDirMissingLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
                }
            }
            headerBrush = m_emptyDirWarningBrush.Get();
            headerLayout = m_emptyDirMissingLayout.Get();
        } else {
            // Directory exists but contains no supported images.
            if (!m_emptyNoImagesLayout) {
                const wchar_t *txt = Constants::Messages::EMPTY_DIR_NO_IMAGES;
                r->m_pDWriteFactory->CreateTextLayout(
                        txt, static_cast<UINT32>(wcslen(txt)),
                        m_emptyFormat.Get(), textW, sh,
                        &m_emptyNoImagesLayout);
                if (m_emptyNoImagesLayout) {
                    m_emptyNoImagesLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    m_emptyNoImagesLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
                }
            }
            headerBrush = r->m_pTextBrush.Get();
            headerLayout = m_emptyNoImagesLayout.Get();
        }

        // Lazily create path layout (per-dir — reset when m_emptyDir changes).
        if (!m_emptyDirPathLayout && !m_emptyDir.empty()) {
            r->m_pDWriteFactory->CreateTextLayout(
                    m_emptyDir.c_str(), static_cast<UINT32>(m_emptyDir.size()),
                    m_emptyFormat.Get(), textW, sh,
                    &m_emptyDirPathLayout);
            if (m_emptyDirPathLayout) {
                m_emptyDirPathLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_emptyDirPathLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
            }
        }

        // Measure both blocks to vertically center the combined text.
        DWRITE_TEXT_METRICS m1{}, m2{};
        if (headerLayout) headerLayout->GetMetrics(&m1);
        if (m_emptyDirPathLayout) m_emptyDirPathLayout->GetMetrics(&m2);

        const float lineGap = 6.0f * app.dpiScale;
        const float totalH = m1.height + lineGap + m2.height;
        const float startY = (sh - totalH) / 2.0f;

        if (headerLayout && headerBrush)
            m_panelContext->DrawTextLayout({margin, startY},
                                           headerLayout,
                                           headerBrush);
        // Path is drawn with the same brush as the header to stay visually consistent.
        if (m_emptyDirPathLayout && headerBrush)
            m_panelContext->DrawTextLayout({margin, startY + m1.height + lineGap},
                                           m_emptyDirPathLayout.Get(),
                                           headerBrush);
    }

    // =========================================================================
    // Unified Renderer Calls — each panel owns its swap chain + device context
    // =========================================================================
    void ThumbnailPanelWnd::CreateDeviceResources() {
        if (!app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (!r) return;

        ID3D11Device *d3dDev = r->GetD3DDevice();
        ID2D1Device6 *d2dDev = r->GetD2DDevice();
        if (!d3dDev || !d2dDev) return;

        // Create a DXGI swap chain bound to this panel's HWND.
        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.BufferCount = 2;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.SampleDesc.Count = 1;

        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(d3dDev->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) return;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return;
        if (FAILED(factory->CreateSwapChainForHwnd(d3dDev, m_hWnd, &swapDesc,
            nullptr, nullptr, &m_swapChain)))
            return;

        // Create a D2D device context for this panel.
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> baseCtx;
        if (FAILED(d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &baseCtx))) return;
        if (FAILED(baseCtx.As(&m_panelContext))) return;
        // All geometry is computed in physical pixels (coords * app.dpiScale).
        // Tell D2D to accept pixel coordinates directly so DirectWrite uses
        // physical-pixel hinting rather than 96-DPI logical hinting.
        m_panelContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        // Size the swap chain and wire the back buffer.
        RECT rc{};
        GetClientRect(m_hWnd, &rc);
        UINT w = static_cast<UINT>(rc.right - rc.left);
        UINT h = static_cast<UINT>(rc.bottom - rc.top);
        if (w == 0 || h == 0) {
            w = 1200;
            h = Constants::THUMBNAIL_PANEL_WINDOW_THICKNESS;
        }
        ResizeSwapChain(w, h);

        // Create brushes using theme colors
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::DarkSlateGray), &m_placeholderBrush);
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::LightGreen), &m_borderBrush);
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(210.0f / 255, 70.0f / 255, 70.0f / 255), &m_emptyDirWarningBrush);
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White), &m_hoverBrush);
        m_panelContext->CreateSolidColorBrush(
                Constants::Theme::ThemedGrayD2D(Constants::Theme::Panel::SCROLLBAR_TRACK,
                                                app.themeFactor, Constants::Theme::Panel::SCROLLBAR_TRACK_ALPHA), &m_scrollTrackBrush);
        m_panelContext->CreateSolidColorBrush(
                Constants::Theme::ThemedGrayD2D(Constants::Theme::Panel::SCROLLBAR_THUMB,
                                                app.themeFactor, Constants::Theme::Panel::SCROLLBAR_THUMB_ALPHA), &m_scrollThumbBrush);
        // Multi-select: steel blue — distinct from green (viewer selection) and white (hover)
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(0.28f, 0.56f, 1.0f), &m_multiSelBrush);
        // Active-panel label: same LightGreen as the selection border — signals focus consistently
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::LightGreen), &m_dirLabelActiveBrush);
        // Inactive label: inverted track gray (0.88 base) — contrasts against both
        // the track (0.12 base) and thumb (0.65 base) in both light and dark themes.
        m_panelContext->CreateSolidColorBrush(
                Constants::Theme::ThemedGrayD2D(0.88f, app.themeFactor, 0.65f),
                &m_dirLabelInactiveBrush);
        // Visual effects brushes (U key master toggle)
        namespace TE = Constants::ThumbnailPanel::ThumbnailEffects;
        m_panelContext->CreateSolidColorBrush(
                D2D1::ColorF(TE::GLOW_COLOR_R, TE::GLOW_COLOR_G, TE::GLOW_COLOR_B, TE::GLOW_COLOR_A),
                &m_glowBrush);
        // Corner bg brush — color is set to clearColor at the start of every Render() call
        m_panelContext->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 1.f), &m_cornerBgBrush);
    }

    // Builds four corner-bite path geometry shapes for thumbnail corner rounding.
    // Each "bite" is the area of a corner square that lies OUTSIDE the quarter-circle arc
    // (the part that would be clipped if we had a rounded rect).  Filled with the panel
    // background color, they overdraw the bitmap corners to produce rounded corners without
    // the cost of a per-thumbnail PushLayer.
    // Geometry is normalized to origin (0,0); caller applies a translation transform.
    void ThumbnailPanelWnd::BuildCornerGeometry(float W, float H, float r) {
        m_cornerGeometry.Reset();
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        m_panelContext->GetFactory(&factory);
        if (!factory) return;

        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom;
        if (FAILED(factory->CreatePathGeometry(&geom))) return;

        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geom->Open(&sink))) return;

        // All four bites share CW / CCW from Microsoft's D2D screen-coords convention:
        //   CW  = angle increases going clockwise on screen (Y down)
        //   Arc directions verified per-corner so the 90° arc passes through the
        //   bite interior (not the exterior 270° long-way arc).
        const auto CW = D2D1_SWEEP_DIRECTION_CLOCKWISE;
        const auto CCW = D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
        const auto SML = D2D1_ARC_SIZE_SMALL;

        // Top-left: (r,0)→(0,0)→(0,r) → arc CW small → (r,0)
        sink->BeginFigure(D2D1::Point2F(r, 0.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(0.f, 0.f));
        sink->AddLine(D2D1::Point2F(0.f, r));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, 0.f), D2D1::SizeF(r, r), 0.f, CW, SML));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        // Top-right: (W-r,0)→(W,0)→(W,r) → arc CCW small → (W-r,0)
        sink->BeginFigure(D2D1::Point2F(W - r, 0.f), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(W, 0.f));
        sink->AddLine(D2D1::Point2F(W, r));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(W - r, 0.f), D2D1::SizeF(r, r), 0.f, CCW, SML));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        // Bottom-right: (W,H-r)→(W,H)→(W-r,H) → arc CCW small → (W,H-r)
        sink->BeginFigure(D2D1::Point2F(W, H - r), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(W, H));
        sink->AddLine(D2D1::Point2F(W - r, H));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(W, H - r), D2D1::SizeF(r, r), 0.f, CCW, SML));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        // Bottom-left: (r,H)→(0,H)→(0,H-r) → arc CW small → (r,H)
        sink->BeginFigure(D2D1::Point2F(r, H), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(0.f, H));
        sink->AddLine(D2D1::Point2F(0.f, H - r));
        sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(r, H), D2D1::SizeF(r, r), 0.f, CW, SML));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);

        if (SUCCEEDED(sink->Close()))
            m_cornerGeometry = std::move(geom);
    }

    void ThumbnailPanelWnd::ResizeSwapChain(UINT w, UINT h) {
        if (!m_swapChain || !m_panelContext) return;

        // Panel width/height changed — text wrap widths are no longer valid.
        // m_emptyFormat encodes a DPI-scaled font size, so also reset it here.
        m_emptyFormat.Reset();
        m_emptyNoImagesLayout.Reset();
        m_emptyDirMissingLayout.Reset();
        m_emptyDirPathLayout.Reset();
        m_emptyCacheLayout.Reset();
        m_dirLabelLayout.Reset();

        m_panelContext->SetTarget(nullptr);
        m_panelBackBuffer.Reset();

        if (FAILED(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) return;

        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) return;

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(m_panelContext->CreateBitmapFromDxgiSurface(surface.Get(), &props,
            &m_panelBackBuffer)))
            return;

        m_panelContext->SetTarget(m_panelBackBuffer.Get());
    }

    void ThumbnailPanelWnd::Render(int selectedIdx, int hoverIdx) {
        if (!m_panelContext || !m_swapChain || !app.renderer) return;
        auto *r = dynamic_cast<RendererD2D *>(app.renderer.get());
        if (!r) return;

        // -------------------------------------------------------------------------
        // Phase 1: cull to visible thumbnails, then resolve bitmap pointers under
        // lock. This avoids mutex acquisition + two hashmap lookups per offscreen
        // thumbnail — critical for large folders (1000 items → ~30 visible).
        // -------------------------------------------------------------------------
        RECT visRect{};
        GetClientRect(m_hWnd, &visRect);
        const float visW = static_cast<float>(visRect.right);
        const float visH = static_cast<float>(visRect.bottom);

        std::vector<size_t> visIdx;
        std::vector<Thumbnail> visThumbs;
        for (size_t i = 0; i < m_thumbnails.size(); ++i) {
            const D2D1_RECT_F &rc = m_thumbnails[i].rect;
            if (rc.right > 0.0f && rc.left < visW && rc.bottom > 0.0f && rc.top < visH) {
                visIdx.push_back(i);
                visThumbs.push_back(m_thumbnails[i]);
            }
        }

        std::vector<RendererD2D::ResolvedThumb> resolved;
        resolved.reserve(visThumbs.size());
        r->ResolveThumbnailBitmaps(visThumbs, UsesDirThumbCache() ? m_hWnd : nullptr, resolved);

        // -------------------------------------------------------------------------
        // Phase 2: GPU draw — no locks held.
        // -------------------------------------------------------------------------
        m_panelContext->BeginDraw();

        // Use active background color if this panel is active
        D2D1_COLOR_F clearColor = Constants::Theme::ThemedGrayD2D(
                Constants::Theme::Panel::BACKGROUND_INACTIVE, app.themeFactor,
                Constants::Theme::Panel::BACKGROUND_INACTIVE_ALPHA);
        if (g_activePanelHwnd == m_hWnd) {
            clearColor = Constants::Theme::ThemedGrayD2D(
                    Constants::Theme::Panel::BACKGROUND_ACTIVE, app.themeFactor,
                    Constants::Theme::Panel::BACKGROUND_ACTIVE_ALPHA);
        }
        m_panelContext->Clear(clearColor);

        // Empty-dir placeholder — drawn instead of thumbnails when the scanned
        // folder contained no images. Fills the full panel area, centered.
        if (m_emptyDirActive && m_thumbnails.empty())
            RenderEmptyPlaceholder();

        // ── Visual effects setup (U key master toggle) ───────────────────────
        namespace TE = Constants::ThumbnailPanel::ThumbnailEffects;
        const bool effectsOn = app.thumbnailEffectsEnabled;

        if (effectsOn && TE::EFFECT_ROUNDED_CORNERS && m_cornerBgBrush)
            m_cornerBgBrush->SetColor(clearColor);

        if (effectsOn && TE::EFFECT_ROUNDED_CORNERS && m_panelContext) {
            if (!m_cornerGeometry || m_cornerGeomDpi != app.dpiScale) {
                const float tw = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
                const float th = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
                const float cr = TE::CORNER_RADIUS * app.dpiScale;
                BuildCornerGeometry(tw, th, cr);
                m_cornerGeomDpi = app.dpiScale;
            }
        }
        // ─────────────────────────────────────────────────────────────────────

        // Rounded-rect helpers: when corner rounding is active, EVERY border
        // (selection glow, hover, multi-select) uses the same radius so the
        // whole strip reads as one consistent design.
        const bool roundedOn = effectsOn && TE::EFFECT_ROUNDED_CORNERS;
        const float cornerR = TE::CORNER_RADIUS * app.dpiScale;
        const auto inflate = [](const D2D1_RECT_F &rc, float d) {
            return D2D1::RectF(rc.left - d, rc.top - d, rc.right + d, rc.bottom + d);
        };
        // Stroke a rect rounded or square depending on the effects state.
        const auto strokeRect = [&](const D2D1_RECT_F &rc, ID2D1Brush *brush,
                                    float thickness, float extraRadius = 0.0f) {
            if (roundedOn)
                m_panelContext->DrawRoundedRectangle(
                        D2D1::RoundedRect(rc, cornerR + extraRadius, cornerR + extraRadius),
                        brush, thickness);
            else
                m_panelContext->DrawRectangle(rc, brush, thickness);
        };

        for (size_t j = 0; j < resolved.size(); ++j) {
            const int origI = static_cast<int>(visIdx[j]);
            const auto &rv = resolved[j];
            const std::wstring &thumbPath = m_thumbnails[origI].filePath;
            const bool isHover = (origI == hoverIdx);
            const bool isSelected = (origI == selectedIdx);
            const float thumbOpacity =
                    (!s_cutPaths.empty() && s_cutPaths.count(thumbPath))
                        ? Constants::ThumbnailPanel::THUMBNAIL_CUT_OPACITY
                        : 1.0f;

            // EFFECT: Hover scale — expand draw rect by HOVER_SCALE_FACTOR around center
            D2D1_RECT_F drawRect = rv.rect;
            if (effectsOn && TE::EFFECT_HOVER_SCALE && isHover) {
                const float s = TE::HOVER_SCALE_FACTOR;
                const float cx = (rv.rect.left + rv.rect.right) * 0.5f;
                const float cy = (rv.rect.top + rv.rect.bottom) * 0.5f;
                const float hw = (rv.rect.right - rv.rect.left) * 0.5f * s;
                const float hh = (rv.rect.bottom - rv.rect.top) * 0.5f * s;
                drawRect = D2D1::RectF(cx - hw, cy - hh, cx + hw, cy + hh);
            }

            if (rv.bitmap) {
                m_panelContext->DrawBitmap(rv.bitmap.Get(), drawRect, thumbOpacity,
                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                m_placeholderBrush->SetOpacity(thumbOpacity);
                m_panelContext->FillRectangle(drawRect, m_placeholderBrush.Get());
                m_placeholderBrush->SetOpacity(1.0f);
            }

            // EFFECT: Rounded corners — overdraw corner bites at the ORIGINAL (unscaled)
            // rect so hover-scale geometry mismatch is imperceptible (only ~5% diff).
            if (effectsOn && TE::EFFECT_ROUNDED_CORNERS && m_cornerGeometry && m_cornerBgBrush) {
                D2D1::Matrix3x2F oldXform;
                m_panelContext->GetTransform(&oldXform);
                m_panelContext->SetTransform(
                        D2D1::Matrix3x2F::Translation(rv.rect.left, rv.rect.top) * oldXform);
                m_panelContext->FillGeometry(m_cornerGeometry.Get(), m_cornerBgBrush.Get());
                m_panelContext->SetTransform(oldXform);
            }

            // Multi-selection: semi-transparent fill + solid border (drawn before
            // the viewer-selection green so green always wins on the same item).
            // Fill + border follow the corner radius when rounding is active.
            if (!m_selectedPaths.empty() && m_selectedPaths.count(thumbPath) && m_multiSelBrush) {
                m_multiSelBrush->SetOpacity(0.22f);
                if (roundedOn)
                    m_panelContext->FillRoundedRectangle(
                            D2D1::RoundedRect(drawRect, cornerR, cornerR), m_multiSelBrush.Get());
                else
                    m_panelContext->FillRectangle(drawRect, m_multiSelBrush.Get());
                m_multiSelBrush->SetOpacity(1.0f);
                strokeRect(drawRect, m_multiSelBrush.Get(),
                           Constants::ThumbnailPanel::SELECTION_BORDER_THICKNESS);
            }

            // EFFECT: Glow border — soft green halo on the selected thumb: three
            // rounded strokes at decreasing opacity fake a blur with zero GPU
            // effect cost. Falls back to the classic flat green border when off.
            if (isSelected) {
                if (effectsOn && TE::EFFECT_GLOW_BORDER && m_glowBrush) {
                    const float d = app.dpiScale;
                    m_glowBrush->SetOpacity(TE::GLOW_OUTER_OPACITY);
                    strokeRect(inflate(drawRect, TE::GLOW_OUTER_OFFSET * d),
                               m_glowBrush.Get(), TE::GLOW_OUTER_STROKE * d,
                               TE::GLOW_OUTER_OFFSET * d);
                    m_glowBrush->SetOpacity(TE::GLOW_MID_OPACITY);
                    strokeRect(inflate(drawRect, TE::GLOW_MID_OFFSET * d),
                               m_glowBrush.Get(), TE::GLOW_MID_STROKE * d,
                               TE::GLOW_MID_OFFSET * d);
                    m_glowBrush->SetOpacity(TE::GLOW_CORE_OPACITY);
                    strokeRect(drawRect, m_glowBrush.Get(), TE::GLOW_CORE_STROKE * d);
                    m_glowBrush->SetOpacity(1.0f);
                } else {
                    strokeRect(drawRect, m_borderBrush.Get(),
                               Constants::ThumbnailPanel::SELECTION_BORDER_THICKNESS);
                }
            }

            if (isHover)
                strokeRect(drawRect, m_hoverBrush.Get(),
                           Constants::ThumbnailPanel::HOVER_THICKNESS);
        }

        // Draw scrollbar when content overflows the visible area.
        if (!m_thumbnails.empty() && m_scrollTrackBrush && m_scrollThumbBrush) {
            RECT cr2{};
            GetClientRect(m_hWnd, &cr2);
            const float sw = static_cast<float>(cr2.right);
            const float sh = static_cast<float>(cr2.bottom);
            const float tw = Constants::THUMBNAIL_PANEL_THUMB_WIDTH * app.dpiScale;
            const float th = Constants::THUMBNAIL_PANEL_THUMB_HEIGHT * app.dpiScale;
            const float sp = Constants::THUMBNAIL_PANEL_THUMB_SPACING * app.dpiScale;
            const float mg = Constants::THUMBNAIL_PANEL_THUMB_MARGIN * app.dpiScale;
            const float n = static_cast<float>(m_thumbnails.size());
            const bool vert = IsVertical();

            const float thumbSz = vert ? th : tw;
            const float content = n * (thumbSz + sp) - sp;
            const float visible = (vert ? sh : sw) - 2.0f * mg;

            if (content > visible) {
                const float BAR = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                const float trackLen = vert ? sh : sw;
                const float barThumb = std::max(Constants::ThumbnailPanel::SCROLLBAR_MIN_THUMB * app.dpiScale,
                                                trackLen * visible / content);
                const float scrollMax = content - visible;
                const float scrollNow = std::clamp(-m_offset, 0.0f, scrollMax);
                const float thumbOff = (scrollMax > 0.0f)
                                           ? (scrollNow / scrollMax) * (trackLen - barThumb)
                                           : 0.0f;

                D2D1_RECT_F track, thumb;
                if (vert) {
                    // Vertical panels (positions 2=right, 4=left)
                    if (m_position == 2) {
                        // Right panel: use SCROLLBAR_POS_RIGHT_PANEL (LEFT)
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL == Constants::ThumbnailPanel::ScrollbarSide::LEFT) {
                            track = D2D1::RectF(0.0f, 0.0f, BAR, sh);
                            thumb = D2D1::RectF(0.0f, thumbOff, BAR, thumbOff + barThumb);
                        } else {
                            track = D2D1::RectF(sw - BAR, 0.0f, sw, sh);
                            thumb = D2D1::RectF(sw - BAR, thumbOff, sw, thumbOff + barThumb);
                        }
                    } else {
                        // Left panel: use SCROLLBAR_POS_LEFT_PANEL (RIGHT)
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL == Constants::ThumbnailPanel::ScrollbarSide::LEFT) {
                            track = D2D1::RectF(0.0f, 0.0f, BAR, sh);
                            thumb = D2D1::RectF(0.0f, thumbOff, BAR, thumbOff + barThumb);
                        } else {
                            track = D2D1::RectF(sw - BAR, 0.0f, sw, sh);
                            thumb = D2D1::RectF(sw - BAR, thumbOff, sw, thumbOff + barThumb);
                        }
                    }
                } else {
                    // Horizontal panels (positions 1=top, 3=bottom)
                    if (m_position == 1) {
                        // Top panel: use SCROLLBAR_POS_TOP_PANEL
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL == Constants::ThumbnailPanel::ScrollbarEdge::TOP) {
                            track = D2D1::RectF(0.0f, 0.0f, sw, BAR);
                            thumb = D2D1::RectF(thumbOff, 0.0f, thumbOff + barThumb, BAR);
                        } else {
                            track = D2D1::RectF(0.0f, sh - BAR, sw, sh);
                            thumb = D2D1::RectF(thumbOff, sh - BAR, thumbOff + barThumb, sh);
                        }
                    } else {
                        // Bottom panel: use SCROLLBAR_POS_BOTTOM_PANEL
                        if (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL == Constants::ThumbnailPanel::ScrollbarEdge::TOP) {
                            track = D2D1::RectF(0.0f, 0.0f, sw, BAR);
                            thumb = D2D1::RectF(thumbOff, 0.0f, thumbOff + barThumb, BAR);
                        } else {
                            track = D2D1::RectF(0.0f, sh - BAR, sw, sh);
                            thumb = D2D1::RectF(thumbOff, sh - BAR, thumbOff + barThumb, sh);
                        }
                    }
                }
                m_panelContext->FillRectangle(track, m_scrollTrackBrush.Get());
                m_panelContext->FillRectangle(thumb, m_scrollThumbBrush.Get());
            }
        }

        // Dir label — drawn AFTER the scrollbar so text is always on top.
        // Active panel: LightGreen. Inactive: inverted-track gray (high contrast
        // against both the track and the thumb regardless of thumb size).
        {
            auto [labelText, boldStart] = BuildScrollbarLabel();
            if (!labelText.empty() && r->m_pDWriteFactory) {
                RECT crl{};
                GetClientRect(m_hWnd, &crl);
                const float sw2 = static_cast<float>(crl.right);
                const float sh2 = static_cast<float>(crl.bottom);
                const float BAR = Constants::ThumbnailPanel::SCROLLBAR_THICKNESS * app.dpiScale;
                const bool vert = IsVertical();
                const float dim = vert ? sh2 : sw2;

                if (!m_dirLabelLayout
                    || labelText != m_dirLabelTextCache
                    || dim != m_dirLabelDimCache
                    || BAR != m_dirLabelBarCache) {
                    m_dirLabelLayout.Reset();
                    m_dirLabelTextCache = labelText;
                    m_dirLabelDimCache = dim;
                    m_dirLabelBarCache = BAR;

                    const float fontSize = std::max(6.0f, BAR - 2.0f);
                    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
                    r->m_pDWriteFactory->CreateTextFormat(
                            L"Segoe UI", nullptr,
                            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL, fontSize,
                            Constants::Overlay::MSG_ALL_FONT_LOCALE,
                            &fmt);

                    if (fmt) {
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        r->m_pDWriteFactory->CreateTextLayout(
                                labelText.c_str(), static_cast<UINT32>(labelText.size()),
                                fmt.Get(), dim, BAR, &m_dirLabelLayout);

                        if (m_dirLabelLayout) {
                            const UINT32 boldLen = static_cast<UINT32>(labelText.size()) - boldStart;
                            if (boldLen > 0)
                                m_dirLabelLayout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD,
                                                                {boldStart, boldLen});
                            m_dirLabelLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
                            r->m_pDWriteFactory->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis);
                            DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                            m_dirLabelLayout->SetTrimming(&trim, ellipsis.Get());
                        }
                    }
                }

                if (m_dirLabelLayout) {
                    ID2D1SolidColorBrush *labelBrush =
                            (m_hWnd == g_activePanelHwnd && m_dirLabelActiveBrush)
                                ? m_dirLabelActiveBrush.Get()
                                : m_dirLabelInactiveBrush.Get();

                    float cx = 0.0f, stripY = 0.0f;
                    if (vert) {
                        if (m_position == 2)
                            cx = (Constants::ThumbnailPanel::SCROLLBAR_POS_RIGHT_PANEL
                                  == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                     ? BAR / 2.0f
                                     : sw2 - BAR / 2.0f;
                        else
                            cx = (Constants::ThumbnailPanel::SCROLLBAR_POS_LEFT_PANEL
                                  == Constants::ThumbnailPanel::ScrollbarSide::LEFT)
                                     ? BAR / 2.0f
                                     : sw2 - BAR / 2.0f;
                    } else {
                        if (m_position == 1)
                            stripY = (Constants::ThumbnailPanel::SCROLLBAR_POS_TOP_PANEL
                                      == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                         ? 0.0f
                                         : sh2 - BAR;
                        else
                            stripY = (Constants::ThumbnailPanel::SCROLLBAR_POS_BOTTOM_PANEL
                                      == Constants::ThumbnailPanel::ScrollbarEdge::TOP)
                                         ? 0.0f
                                         : sh2 - BAR;
                    }

                    D2D1::Matrix3x2F oldXform;
                    m_panelContext->GetTransform(&oldXform);

                    if (vert) {
                        m_panelContext->SetTransform(
                                D2D1::Matrix3x2F::Rotation(-90.0f, D2D1::Point2F(cx, sh2 / 2.0f)));
                        m_panelContext->DrawTextLayout(
                                D2D1::Point2F(cx - sh2 / 2.0f, sh2 / 2.0f - BAR / 2.0f),
                                m_dirLabelLayout.Get(), labelBrush);
                    } else {
                        m_panelContext->DrawTextLayout(
                                D2D1::Point2F(0.0f, stripY),
                                m_dirLabelLayout.Get(), labelBrush);
                    }

                    m_panelContext->SetTransform(oldXform);
                }
            }
        }

        HRESULT hr = m_panelContext->EndDraw();
        if (hr != D2DERR_RECREATE_TARGET && hr != static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED))
            m_swapChain->Present(0, 0);
    }
} // namespace UI
