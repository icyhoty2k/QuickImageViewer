// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#pragma once
#include <windows.h>
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include "Platform/Constants.h"

// =============================================================================
// PromotionPlaylist — the SECOND playlist used by dedicated instances.
//
// DESIGN RULE: promotions are never merged into app.playlist. This class owns a
// completely separate list of files, and the established image pipeline is not
// aware of it. That keeps the image counter, arrow-key navigation, sorting and
// the async decode guard behaving exactly as before; a promotion is simply a
// path this class hands out when it is that path's turn to be shown.
//
// PRIORITY: every promotion carries a weight of 1..65535 read from a "#<n>"
// suffix on the file stem — "summer-sale#500.jpg" has weight 500, a file with
// no suffix has weight 1. Under WEIGHTED order a promo's chance of being drawn
// is its weight divided by the total, so 500 is 500× likelier than 1.
//
// SPACING: the caller ticks this once per displayed image; after a re-rolled
// gap of [everyMin..everyMax] images, ShouldShowNow() comes back true.
// =============================================================================

namespace Dedicated {

struct PromotionEntry {
    std::wstring path;
    uint16_t     weight = Constants::Dedicated::PROMO_WEIGHT_DEFAULT;
};

class PromotionPlaylist {
    public:
        PromotionPlaylist();

        // Scans `folder` for images, reading each file's weight from its name.
        // Returns false when the folder is missing or holds no usable image.
        // Safe to call again to refresh; always replaces the previous contents.
        bool Scan(const std::wstring &folder);

        // Scans SEVERAL folders into one pool. This is the form a dedicated
        // instance uses: its promotions come from promotionList_*.qpr, which
        // holds any number of folders. Missing folders are skipped, not fatal.
        bool Scan(const std::vector<std::wstring> &folders);

        void Clear();

        bool                 IsEmpty()     const { return m_entries.empty(); }
        size_t               Count()       const { return m_entries.size(); }
        // First scanned folder, for display. With several folders this is only
        // a label — Count() is the meaningful figure.
        const std::wstring  &Folder()      const { return m_folder; }
        uint64_t             TotalWeight() const { return m_totalWeight; }
        const std::vector<PromotionEntry> &Entries() const { return m_entries; }

        // --- Selection -------------------------------------------------------
        // The NEXT promotion is decided in advance so exactly one can be kept
        // warm in the bitmap cache. Peek to preload it; Take to consume it,
        // which immediately decides the following one for the next preload.
        // Both return nullptr when the list is empty.
        const PromotionEntry *PeekNext() const;
        const PromotionEntry *TakeNext();

        // Chooses which promotion comes next (Constants::Dedicated::PromoOrder).
        // Set once from config; Scan and TakeNext use it to refresh the peek.
        void SetOrder(int order);
        int  Order() const { return m_order; }

        // --- Triggers (WHEN a promotion is due) ------------------------------
        // Each is a (from, to) pair — see the rules on
        // Constants::Dedicated. In short: from == 0 disables, to == 0 means a
        // strict cadence of `from`, otherwise the gap is re-rolled in [from,to].
        // The two triggers are INDEPENDENT: each keeps its own countdown and
        // re-arms only itself.
        void SetImageTrigger(int from, int to);  // counted in images shown
        void SetTimeTrigger(int fromSec, int toSec); // counted in seconds

        bool ImageTriggerEnabled() const { return m_imgFrom > 0; }
        bool TimeTriggerEnabled()  const { return m_secFrom > 0; }
        bool TriggersEnabled()     const { return ImageTriggerEnabled() || TimeTriggerEnabled(); }
        bool IsActive()            const { return !m_entries.empty() && TriggersEnabled(); }

        int ImageFrom() const { return m_imgFrom; }
        int ImageTo()   const { return m_imgTo; }
        int TimeFrom()  const { return m_secFrom; }
        int TimeTo()    const { return m_secTo; }

        // Call once per displayed IMAGE — never for a promotion, since a
        // promotion must not count toward the gap before the next one.
        // Returns true when either trigger has come due; only the trigger that
        // fired is re-armed. If both come due on the same image, ONE promotion
        // is shown and both re-arm.
        bool ShouldShowNow();

        // Re-arms both triggers without consuming a tick. Call when the
        // slideshow starts, a trigger changes, or the folder is rescanned.
        void Rearm();

    private:
        const PromotionEntry *Decide();  // applies m_order to choose one entry
        void RefreshPeek();              // decides the upcoming promotion
        int  Roll(int from, int to);     // (from,0)=strict from, else [from,to]
        void ArmImages();
        void ArmTime();

        std::wstring                m_folder;
        std::vector<PromotionEntry> m_entries;
        uint64_t                    m_totalWeight = 0; // 64-bit: 65535 × many files
        int                         m_seqIndex    = 0;
        int                         m_order = Constants::Dedicated::PromoOrder::WEIGHTED;

        // Index of the promotion decided in advance, -1 when none.
        int m_peekIndex = -1;

        int m_imgFrom = Constants::Dedicated::PROMO_IMAGES_EVERY_DEFAULT;
        int m_imgTo   = Constants::Dedicated::PROMO_IMAGES_UPTO_DEFAULT;
        int m_secFrom = Constants::Dedicated::PROMO_SECONDS_EVERY_DEFAULT;
        int m_secTo   = Constants::Dedicated::PROMO_SECONDS_UPTO_DEFAULT;

        int       m_imgCountdown = 0; // images left before the image trigger fires
        ULONGLONG m_nextDueTick  = 0; // GetTickCount64 the time trigger is due

        std::mt19937 m_rng;
};

// Parses the "#<n>" weight suffix from a file STEM (no extension).
// Returns PROMO_WEIGHT_DEFAULT when absent or out of range. Exposed for tests
// and for the UI that previews what a folder's weights will be.
uint16_t ParsePromotionWeight(const std::wstring &stem);

} // namespace Dedicated
