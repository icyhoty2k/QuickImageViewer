// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once

#include "IRenderer.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_3.h>
#include <d2d1svg.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <vector>

#include "../UI/ThumbnailPanels/Thumbnail.h"
#include "WorkerThread.h"
#include "Platform/ConstantsTheme.h"

class RendererD2D final : public IImageRenderer {
    public:
        int g_hoverIndex = -1;

        RendererD2D() = default;

        ~RendererD2D() override = default;

        const wchar_t* GetName() const override { return L"Direct2D"; }
        [[nodiscard]] HRESULT Initialize(HWND hwnd) override;

        void Resize(UINT width, UINT height) override;

        [[nodiscard]] HRESULT LoadBitmap(IWICBitmapSource *bitmap, UINT width, UINT height, const std::wstring &filePath) override;

        [[nodiscard]] HRESULT PreloadBitmap(const std::wstring &filePath, int requestIndex, int expectedCurrentIndex = -1) override;

        [[nodiscard]] HRESULT Render() override;

        void UpdateColorEffects() override;

        [[nodiscard]] HRESULT SaveCurrentImageWithEffects(const std::wstring &outPath) override;

        void ClearActiveImage() override;

        // SVG support
        [[nodiscard]] HRESULT PreloadSvgFromBytes(std::vector<BYTE> svgBytes,
                                                  const std::wstring &filePath,
                                                  int requestIndex) override;

        bool HasActiveSvg() const override {
            return m_pActiveSvg != nullptr;
        }

        // Cache Management
        std::vector<CacheItem> GetCachedBitmaps() override;
        USHORT GetCachedOrientation(const std::wstring &filePath) override;

        void ClearCache() override;

        void ClearCache(const std::wstring &excludePath) override;

        void RemoveFromCache(const std::wstring &filePath) override;

        void GetImageCacheStats(int &count, UINT64 &estimatedBytes) override;
        void GetDirThumbCacheStats(int &count, UINT64 &estimatedBytes) override;

        // =====================================================================
        // Thumbnail Panel APIs
        // Each ThumbnailPanelWnd owns its own swap chain — these two accessors
        // let panels borrow the shared D3D/D2D device for one-time init.
        // =====================================================================
        ID3D11Device *GetD3DDevice() const { return m_pD3DDevice.Get(); }
        ID2D1Device6 *GetD2DDevice() const { return m_pD2DDevice.Get(); }

        // Resolve bitmap pointers for a set of thumbnails under the minimum lock
        // duration. Called by ThumbnailPanelWnd::Render() before GPU work begins.
        // hPanel = panel HWND → look in that panel's private dir-thumb cache (DirWnd)
        // hPanel = nullptr   → look in m_bitmapCache only (CacheWnd)
        struct ResolvedThumb {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap; // nullptr = placeholder
            D2D1_RECT_F rect;
        };
        void ResolveThumbnailBitmaps(const std::vector<UI::Thumbnail> &thumbnails,
                                     HWND hPanel,
                                     std::vector<ResolvedThumb> &out);

        // Queues an async decode+scale job for one file.
        // Posts WM_QIV_REPAINT to hPanel when the thumbnail lands.
        void RequestDirThumbnail(const std::wstring &filePath, HWND hPanel);

        // Drops the thumbnail cache that belongs to hPanel only.
        // Other panels are completely unaffected.
        void ClearDirThumbnailCache(HWND hPanel);

        void ApplyPreviousEffects() override;

        // Public DWrite resources
        Microsoft::WRL::ComPtr<IDWriteFactory3> m_pDWriteFactory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> m_pTextFormat;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pTextBrush;

    private:
        // Device-independent resources
        Microsoft::WRL::ComPtr<ID2D1Factory7> m_pD2DFactory;
        // --- The Master Bypass Switch (Renderer-side) ---
        // This acts as the final "Display Node" that Render() draws.
        // It points to EITHER m_pBitmap (bypass) or the effect output (active).
        Microsoft::WRL::ComPtr<ID2D1Image> m_pActiveDisplayNode;
        // Device-dependent resources
        Microsoft::WRL::ComPtr<ID3D11Device> m_pD3DDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pD3DContext;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_pSwapChain;
        Microsoft::WRL::ComPtr<ID2D1Device6> m_pD2DDevice;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext7> m_pDeviceContext;

        // =====================================================================
        // NOTE: No shared decode/thumbnail DeviceContexts here.
        // ID2D1Device::CreateDeviceContext() is thread-safe; each worker task
        // creates its own short-lived DeviceContext from m_pD2DDevice, uses it,
        // and releases it. This eliminates all data races on shared D2D state.

        // ID2D1DeviceContext5 is queried once from m_pDeviceContext and cached
        // here to avoid a per-frame QueryInterface in the SVG render path.
        // Reset in DiscardDeviceResources(), repopulated in CreateDeviceResources().
        Microsoft::WRL::ComPtr<ID2D1DeviceContext5> m_pDeviceContext5;

        // Continuous adjustments only: saturation + contrast + brightness folded
        // into one 5x4 color matrix computed explicitly in UpdateColorEffects().
        // This node is ALWAYS first in the chain (it is a per-image correction,
        // not a stackable toggle).
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pColorMatrixEffect;
        // Toggle effects. Each is its own node so BuildEffectChain() can place it
        // anywhere in the chain — the position comes from app.activeEffectsList,
        // i.e. the order the user switched them on.
        // Grayscale/invert/sepia used to be folded into m_pColorMatrixEffect,
        // which pinned them BEFORE every non-linear node — and since the
        // solarize LUT is invariant under inversion (solarize(1-v) == solarize(v)),
        // toggling Invert after Solarize was a visual no-op. Keep them separate.
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pGrayscaleEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pInvertEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSepiaEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pGammaEffect; // CLSID_D2D1GammaTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pSolarizeEffect; // CLSID_D2D1TableTransfer
        // Threshold is two nodes: luminance collapse, then the 0/1 table.
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pThresholdLumaEffect; // CLSID_D2D1ColorMatrix
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pThresholdEffect; // CLSID_D2D1TableTransfer
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pOutlineEffect; // CLSID_D2D1EdgeDetection
        Microsoft::WRL::ComPtr<ID2D1Effect> m_pScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBackBufferBitmap;

        // Per-panel dir-thumbnail cache.
        // Each DirWnd / SpawnedDirWnd owns its own entry keyed by its HWND,
        // so clearing one panel's cache never evicts thumbnails owned by another.
        //
        // BOUNDED BY BYTES, not by count — see EvictPanelThumbs.
        //
        // This cache had no eviction at all: thumbnails were inserted and only
        // ever dropped when the whole panel changed folder. Scrolling one panel
        // through a large folder therefore grew without limit — at 200% DPI a
        // thumbnail is 256x160x4, about 160 KB, so a 10,000-image folder is
        // roughly 1.6 GB of GPU memory, times up to five panels.
        //
        // app.dirThumbCacheMB existed the whole time — persisted, in the tray
        // menu, adjustable from 100 to 64000 MB, shown in the Stats panel — and
        // was read by nothing. Lowering it changed nothing. It is the budget now.
        //
        // Byte-exact rather than estimated: the size is known at creation from
        // the dimensions the thumbnail was actually made at, so DPI changes and
        // the odd non-standard thumbnail are accounted correctly.
        struct PanelThumbEntry {
            std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap1>> bitmaps;
            std::unordered_set<std::wstring> inFlight;

            // Position of each of this panel's thumbnails in the GLOBAL lru list
            // below, plus what each one costs. The ordering itself is not kept
            // here — see m_thumbLru for why.
            std::unordered_map<std::wstring, std::list<std::pair<HWND, std::wstring>>::iterator> lruPos;
            std::unordered_map<std::wstring, size_t> bytes;
            size_t totalBytes = 0; // this panel's share, for reporting
        };
        std::unordered_map<HWND, PanelThumbEntry> m_panelThumbCaches;
        std::mutex m_dirThumbMutex;

        // ONE LRU ACROSS ALL PANELS, most-recently-drawn at the FRONT.
        //
        // The budget is a single number the user sets, so it has to be enforced
        // against a single total. The obvious alternative — give each panel
        // budget/N — is wrong twice over: hiding a panel does not clear its
        // cache, so an unwatched strip would still reserve its share; and panels
        // open and close constantly, so every change would evict everyone else
        // down and then hand the space back, churning for nothing.
        //
        // A shared list needs no divisor and no panel count. Thumbnails are
        // promoted when they are DRAWN, so a panel nobody is looking at simply
        // stops being touched and drifts to the back, where it is evicted first.
        // The strips in front of the user keep their thumbnails because they are
        // the ones being used. That is the behaviour a shared budget should have,
        // and it falls out of the ordering instead of being computed.
        std::list<std::pair<HWND, std::wstring>> m_thumbLru;
        size_t m_thumbTotalBytes = 0;

        // Drops least-recently-used thumbnails until EVERY panel is inside its
        // share of app.dirThumbCacheMB. The budget is shared between the five
        // strips, not granted to each — see the definition.
        // Caller must already hold m_dirThumbMutex.
        void EnforceThumbBudget();

        // Marks a thumbnail as just-used. Caller must hold m_dirThumbMutex.
        void TouchPanelThumb(PanelThumbEntry &entry, const std::wstring &path);

        // Cache
        struct CachedBitmap {
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
            std::list<std::wstring>::iterator lruIt;
            UINT  width       = 0;
            UINT  height      = 0;
            USHORT orientation = 1; // EXIF tag 274: 1=normal, 3=180°, 6=90°CW, 8=270°CW, etc.
            // Animated GIF: all composited frames + per-frame delays in ms.
            // Empty for non-animated images.
            std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> gifFrames;
            std::vector<int> gifDelays;
        };

        Microsoft::WRL::ComPtr<ID2D1Bitmap1> m_pBitmap;
        std::unordered_map<std::wstring, CachedBitmap> m_bitmapCache;
        std::list<std::wstring> m_lruList;
        std::unordered_set<std::wstring> m_bitmapInFlight;
        std::mutex m_cacheMutex;

        // Evict LRU entries until there is room for ONE more, then return.
        // The CALLER MUST HOLD m_cacheMutex and is about to push_front.
        //
        // This was open-coded at four insert sites as
        //   if (m_lruList.size() >= vramCacheCount) { erase(back()); pop_back(); }
        // which had two faults. vramCacheCount is floored at 0 by both setters,
        // so `size() >= 0` was always true and back() ran on an EMPTY list — a
        // crash the moment an image is opened with the cache set to 0. And a lone
        // `if` evicts only one per insert, so lowering the limit from 900 to 10
        // at runtime left the cache oversized, shrinking one entry per image
        // instead of at once. One while-loop fixes both, and having one copy
        // means the next change cannot land in three places and miss the fourth.
        //
        // The effective floor is 1, not 0: the caller unconditionally pushes one
        // entry after this, so a true 0 is not reachable from here without moving
        // the push too. Bounding at 1 keeps the degenerate "cache 0" setting to a
        // single resident bitmap rather than letting the list grow without limit,
        // which a plain `>= 0` loop would do.
        //
        // Defined in the .cpp, NOT inline: it reads app.vramCacheCount, and this
        // header deliberately does not include AppState.h (see the note by
        // m_lastFolderOverlayState for why). The .cpp already has it.
        void EvictCacheToMakeRoom();

        // Animated GIF runtime state (active image only)
        std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> m_gifFrames;
        std::vector<int> m_gifDelays;
        int m_gifFrame = 0;

    public:
        bool IsAnimatedGif()        const override { return !m_gifFrames.empty(); }
        int  GetCurrentGifDelay()   const override;
        int  AdvanceGifFrame()            override;
        void ResetGifAnimation()          override;
    private:

        // Persistent overlay (Missing / Empty) — drawn until user opens a new folder.
        // Format + layout are DWrite (device-independent): created once, survive
        // device loss and resize — only the brush is device-dependent.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pFolderDeletedBrush;   // red — Missing state
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pLinkBrush;            // Constants::Links — clickable path
        // The placeholder's own two colours: the message, and the version line
        // under it. m_pTextBrush is left alone — it is the overlay manager's.
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pPlaceholderBrush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_pPlaceholderDimBrush;
        Microsoft::WRL::ComPtr<IDWriteTextFormat>     m_pFolderOverlayFormat;  // created lazily, cached forever
        Microsoft::WRL::ComPtr<IDWriteTextLayout>     m_pFolderDeletedLayout;
        // What the cached layout was built FROM. Stored as the two parts rather
        // than as one composite key string: the key used to be rebuilt by
        // concatenation on every frame purely to compare it, which is a heap
        // allocation per frame for a value that changes only when the user lands
        // in a different folder. Two plain comparisons cost nothing.
        //
        // The path is the ONLY variable in this placeholder. The format, both
        // brushes and the link styling are created once and cached forever.
        // Recomputes app.folderOverlayPathRect — the clickable region of the
        // second line — from the CURRENT layout geometry.
        //
        // Separate from building the layout because the two go stale for
        // different reasons. The layout depends on the text; the rect also
        // depends on the window size, and a resize re-flows the cached layout
        // without rebuilding it. Computed in one place so the two cannot
        // disagree about where that line ended up.
        void UpdateFolderOverlayPathRect();

    public:
        // Did the decoder try this file and give up?
        //
        // The worker abandons a file it cannot read with a bare `return` from
        // any of a dozen places — no bitmap, no message, nothing. That is
        // correct for a stale request, but for a file that is genuinely
        // undecodable it left the UI waiting forever for a decode that had
        // already failed, which is what a .txt renamed to .jpg produced: a
        // permanently black window.
        //
        // Recorded per path so the UI can say so instead. Cleared when the file
        // is asked for again, since the file on disk may have been replaced.
        bool DecodeFailed(const std::wstring &path) const override;

        // Public because callers OUTSIDE the decode path need it — the SVG route
        // reads its own bytes on the IO worker and has to be able to report a
        // file it could not read. NoteDecodeFailure below is the implementation
        // both this and the internal decode failures go through, so there is one
        // record and one wake-up, not two.
        void MarkDecodeFailed(const std::wstring &path) override { NoteDecodeFailure(path); }

    private:
        void NoteDecodeFailure(const std::wstring &path);

        mutable std::mutex                m_failedMutex;
        std::unordered_set<std::wstring>  m_failedPaths;

        // Heading and last line for the current state, in one place each, so the
        // layout text, the character offsets and the hit-testing cannot disagree
        // about how long the heading is or what the last line says.
        static const wchar_t        *FolderOverlayHeader();
        static const wchar_t        *FolderOverlayHeaderIcon();
        static const std::wstring   &FolderOverlayLastLine();

        // The placeholder's four lines — heading, open prompt, path, version —
        // composed once, with the character ranges of the two clickable spans.
        //
        // ONE function so the drawn text and the click targets cannot drift
        // apart. They are computed from the same offsets in the same pass: any
        // other arrangement means a change to the wording silently moves the
        // link somewhere the text no longer is, and nothing reports it because
        // both halves still look correct on their own.
        //
        // The key hint at the end of a line is part of that line's target: the
        // whole thing, words and hint, is one link. So each line centres on its
        // full content, which is what DWrite does on its own — there is nothing
        // to compensate for and no padding anywhere in here.
        //
        // The hint carries its own range only so the three spaces between it
        // and the words can be left un-underlined. The click region spans from
        // the words through the hint, gap included, because a target with a
        // hole in the middle of it is worse than a slightly generous one.
        struct FolderOverlayText {
            std::wstring text;
            UINT32       pathStart       = 0, pathLen       = 0; // line 2, folder or file
            UINT32       pathHintStart   = 0, pathHintLen   = 0; // line 2, "(L)"
            UINT32       actionStart     = 0, actionLen     = 0; // line 3, the prompt
            UINT32       actionHintStart = 0, actionHintLen = 0; // line 3, "(F2)"
            UINT32       versionStart    = 0, versionLen    = 0; // last line, dimmed and shrunk
        };
        static FolderOverlayText BuildFolderOverlayText();

        // What the cached layout was built from, kept alongside it.
        //
        // The hit-testing needs the character ranges, and it runs on resize as
        // well as on rebuild. Recomposing them there would allocate a whole
        // string per call to recover offsets that were already computed — and
        // worse, it would be a second copy of the composition that could
        // disagree with the one on screen. Written in exactly one place: right
        // after the layout it describes is created.
        FolderOverlayText m_folderOverlayComposed;

        std::wstring                                  m_lastFolderOverlayPath;   // the LAST LINE it was built from
        // AppState::FolderOverlayState as an int, deliberately.
        //
        // This header is included by half the UI, and AppState.h is one of the
        // heaviest in the project — naming the enum here would pull it into
        // every one of those translation units to store four bits of cache key.
        // -1 is "nothing cached yet"; it matches no enumerator.
        int                                           m_lastFolderOverlayState   = -1;
        // THE THIRD CACHE INPUT, and the one that was missing. The text format is
        // built at MSG_CENTER_FONT_SIZE * app.dpiScale and the version line is
        // sized the same way, so a move to a monitor at a different scaling
        // changes what this placeholder should look like — while neither the
        // state nor the path has changed, which is all the key used to compare.
        // 0 never matches a real scale, so the first frame always builds.
        float                                         m_lastFolderOverlayDpi     = 0.0f;
        bool                                          m_lastFolderOverlayValid   = false;

        // SVG: active D2D SVG document (legacy path, never set by resvg;
        // resvg-rasterized SVGs go into m_bitmapCache as bitmaps instead)
        Microsoft::WRL::ComPtr<ID2D1SvgDocument> m_pActiveSvg;
        float m_svgNativeW = 0.0f;
        float m_svgNativeH = 0.0f;

        // Worker-safe SVG rasterization: parse + render + GPU upload only.
        // Writes no shared state; the caller inserts outBmp into m_bitmapCache.
        // Uses the injected per-thread WIC factory (wicFac) — never app.wicFactory.
        [[nodiscard]] HRESULT DecodeSvgToBitmap(const std::vector<BYTE> &svgBytes,
                                                IWICImagingFactory2 *wicFac,
                                                Microsoft::WRL::ComPtr<ID2D1Bitmap1> &outBmp,
                                                UINT &outW, UINT &outH);

        // Window state
        HWND m_hwnd = nullptr;
        D2D1_COLOR_F m_clearColor = D2D1::ColorF(
                Constants::Theme::Background::MAIN_WINDOW,
                Constants::Theme::Background::MAIN_WINDOW,
                Constants::Theme::Background::MAIN_WINDOW);

        void SetThemeFactor(float factor) override {
            const float v = Constants::Theme::Apply(Constants::Theme::Background::MAIN_WINDOW, factor);
            m_clearColor = D2D1::ColorF(v, v, v);
        }

        // Internal helpers
        HRESULT CreateDeviceResources();

        void DiscardDeviceResources();

        HRESULT CreateBackBufferBitmap();

        // Creates the toggle effect nodes (grayscale/invert/sepia/gamma/
        // solarize/threshold/outline) lazily on first use, since most images
        // never touch them.
        HRESULT EnsureExtraEffects();

        // Wires app's saturation/contrast/brightness into m_pColorMatrixEffect
        // (always first), then gamma, then chains every toggled-on effect in
        // app.activeEffectsList order — the order the user switched them on, so
        // each effect operates on the result of the ones before it.
        // Returns the final ID2D1Image* ready to be scaled/drawn or captured.
        ID2D1Effect *BuildEffectChain(ID2D1Image *source);

        // Appends one effect's node(s) to `current` and returns the new tail.
        ID2D1Effect *ChainEffectByName(const std::wstring &name, ID2D1Effect *current);

        // Clears input 0 on every effect node so a bypassed graph holds no
        // reference to the source bitmap.
        void ReleaseEffectInputs();
};

// Declare the globals so all files see them
extern DecoderThreadPool g_decoderWorker;
extern IoThreadPool g_ioWorker;
extern DecoderThreadPool g_dirThumbWorker;
