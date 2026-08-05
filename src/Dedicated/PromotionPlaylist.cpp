// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Ivan Hristov Yanev
//
// This file is part of QuickImageViewer. It is free software: you may
// redistribute and modify it under the terms of the GNU Affero General Public
// License version 3 or later, as published by the Free Software Foundation.
// It is distributed WITHOUT ANY WARRANTY. See the LICENSE file for details.

#include "PromotionPlaylist.h"
#include "Platform/FileHandler.h" // is_image_ext — one shared definition of "an image"
#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace Dedicated {

// =============================================================================
// ParsePromotionWeight
// =============================================================================
uint16_t ParsePromotionWeight(const std::wstring &stem) {
    namespace D = Constants::Dedicated;

    const size_t marker = stem.find_last_of(D::PROMO_WEIGHT_MARKER);
    if (marker == std::wstring::npos || marker + 1 >= stem.size())
        return static_cast<uint16_t>(D::PROMO_WEIGHT_DEFAULT);

    const std::wstring digits = stem.substr(marker + 1);
    if (digits.size() > 5) return static_cast<uint16_t>(D::PROMO_WEIGHT_DEFAULT);
    for (wchar_t c : digits)
        if (c < L'0' || c > L'9') return static_cast<uint16_t>(D::PROMO_WEIGHT_DEFAULT);

    // Accumulate in a wide type: the text could still say 99999.
    unsigned long value = 0;
    for (wchar_t c : digits) value = value * 10 + static_cast<unsigned long>(c - L'0');

    if (value < static_cast<unsigned long>(D::PROMO_WEIGHT_MIN) ||
        value > static_cast<unsigned long>(D::PROMO_WEIGHT_MAX))
        return static_cast<uint16_t>(D::PROMO_WEIGHT_DEFAULT);
    return static_cast<uint16_t>(value);
}

// =============================================================================
// Construction / scanning
// =============================================================================
PromotionPlaylist::PromotionPlaylist()
    : m_rng(std::random_device{}()) {
    Rearm();
}

void PromotionPlaylist::Clear() {
    m_folder.clear();
    m_entries.clear();
    m_totalWeight = 0;
    m_seqIndex = 0;
    m_peekIndex = -1;
    Rearm();
}

bool PromotionPlaylist::Scan(const std::wstring &folder) {
    Clear();
    if (folder.empty()) return false;

    std::error_code ec;
    if (!fs::is_directory(folder, ec) || ec) return false;

    // Non-recursive on purpose: a promotions folder is a flat, curated set, and
    // recursing would quietly pull in unrelated images.
    for (const auto &entry : fs::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;

        const fs::path &p = entry.path();
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (!is_image_ext(ext)) continue;

        PromotionEntry pe;
        pe.path   = p.wstring();
        pe.weight = ParsePromotionWeight(p.stem().wstring());
        m_totalWeight += pe.weight;
        m_entries.push_back(std::move(pe));
    }

    if (m_entries.empty()) {
        m_totalWeight = 0;
        return false;
    }

    // Stable, predictable order so SEQUENTIAL matches what the user sees in
    // Explorer rather than whatever order the filesystem handed back.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const PromotionEntry &a, const PromotionEntry &b) {
                  return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
              });

    m_folder = folder;
    RefreshPeek(); // decide the first promotion so it can be preloaded at once
    Rearm();
    return true;
}

bool PromotionPlaylist::Scan(const std::vector<std::wstring> &folders) {
    Clear();
    if (folders.empty()) return false;

    std::error_code ec;
    for (const std::wstring &folder : folders) {
        if (folder.empty()) continue;
        ec.clear();
        // A missing folder is skipped, not fatal: one unplugged drive must not
        // blank a screen whose other folders are fine.
        if (!fs::is_directory(folder, ec) || ec) continue;

        for (const auto &entry : fs::directory_iterator(folder, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) continue;

            const fs::path &p = entry.path();
            std::wstring ext = p.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (!is_image_ext(ext)) continue;

            PromotionEntry pe;
            pe.path   = p.wstring();
            pe.weight = ParsePromotionWeight(p.stem().wstring());
            m_totalWeight += pe.weight;
            m_entries.push_back(std::move(pe));
        }
        if (m_folder.empty()) m_folder = folder; // first that yielded anything
    }

    if (m_entries.empty()) {
        m_totalWeight = 0;
        m_folder.clear();
        return false;
    }

    // Sorted across ALL folders so SEQUENTIAL is deterministic regardless of
    // the order the list file happens to name them in.
    std::sort(m_entries.begin(), m_entries.end(),
              [](const PromotionEntry &a, const PromotionEntry &b) {
                  return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
              });

    RefreshPeek();
    Rearm();
    return true;
}

// =============================================================================
// Selection
//
// The next promotion is decided AHEAD of being needed, so the caller can keep
// exactly one promo bitmap warm in the cache alongside the one upcoming normal
// image. Decide() picks, RefreshPeek() stores the choice, TakeNext() consumes
// it and immediately decides the following one.
// =============================================================================
const PromotionEntry *PromotionPlaylist::Decide() {
    if (m_entries.empty()) return nullptr;

    if (m_order == Constants::Dedicated::PromoOrder::SEQUENTIAL) {
        if (m_seqIndex < 0 || m_seqIndex >= static_cast<int>(m_entries.size())) m_seqIndex = 0;
        const PromotionEntry *e = &m_entries[m_seqIndex];
        m_seqIndex = (m_seqIndex + 1) % static_cast<int>(m_entries.size());
        return e;
    }

    // WEIGHTED — one uniform draw across the summed weights, then walk the
    // cumulative total. O(n) per pick, nothing for a curated folder, and it
    // avoids a prefix table that Scan would have to rebuild.
    if (m_totalWeight == 0) return nullptr;
    std::uniform_int_distribution<uint64_t> dist(1, m_totalWeight);
    uint64_t roll = dist(m_rng);
    for (const PromotionEntry &e : m_entries) {
        if (roll <= e.weight) return &e;
        roll -= e.weight;
    }
    return &m_entries.back(); // unreachable unless weights changed mid-walk
}

void PromotionPlaylist::RefreshPeek() {
    const PromotionEntry *e = Decide();
    m_peekIndex = e ? static_cast<int>(e - m_entries.data()) : -1;
}

void PromotionPlaylist::SetOrder(int order) {
    m_order = (order == Constants::Dedicated::PromoOrder::SEQUENTIAL)
                  ? Constants::Dedicated::PromoOrder::SEQUENTIAL
                  : Constants::Dedicated::PromoOrder::WEIGHTED;
    RefreshPeek(); // the pending choice must reflect the new order
}

const PromotionEntry *PromotionPlaylist::PeekNext() const {
    if (m_peekIndex < 0 || m_peekIndex >= static_cast<int>(m_entries.size())) return nullptr;
    return &m_entries[m_peekIndex];
}

const PromotionEntry *PromotionPlaylist::TakeNext() {
    const PromotionEntry *e = PeekNext();
    RefreshPeek(); // decide the following one so it can be preloaded now
    return e;
}

// =============================================================================
// Triggers
//
// Pair rules (both triggers, see Constants::Dedicated):
//   from == 0            → trigger OFF, whatever `to` says
//   to   == 0            → STRICT: exactly every `from`
//   to   >  from         → RANDOM: re-rolled in [from, to]
//   to   <= from, non-0 → tolerated, falls back to STRICT `from`
// =============================================================================
namespace {
    // Normalises one (from, to) pair against a maximum. A zero/negative `from`
    // collapses the whole pair to disabled — a half-filled pair such as (0, 90)
    // is a config mistake, and inventing a 0..90 range would fire nonstop.
    void NormalizePair(int &from, int &to, int maxValue) {
        if (from <= 0) { from = 0; to = 0; return; }
        from = std::clamp(from, 1, maxValue);
        if (to <= 0 || to < from) to = 0;   // strict cadence at `from`
        else                      to = std::clamp(to, from, maxValue);
    }
}

void PromotionPlaylist::SetImageTrigger(int from, int to) {
    m_imgFrom = from;
    m_imgTo   = to;
    NormalizePair(m_imgFrom, m_imgTo, Constants::Dedicated::PROMO_IMAGES_MAX);
    ArmImages();
}

void PromotionPlaylist::SetTimeTrigger(int fromSec, int toSec) {
    m_secFrom = fromSec;
    m_secTo   = toSec;
    NormalizePair(m_secFrom, m_secTo, Constants::Dedicated::PROMO_SECONDS_MAX);
    ArmTime();
}

int PromotionPlaylist::Roll(int from, int to) {
    if (from <= 0) return 0;
    if (to <= from) return from; // strict
    std::uniform_int_distribution<int> dist(from, to);
    return dist(m_rng);
}

void PromotionPlaylist::ArmImages() {
    m_imgCountdown = Roll(m_imgFrom, m_imgTo);
}

void PromotionPlaylist::ArmTime() {
    const int secs = Roll(m_secFrom, m_secTo);
    m_nextDueTick = secs > 0
                        ? GetTickCount64() + static_cast<ULONGLONG>(secs) * 1000ull
                        : 0;
}

void PromotionPlaylist::Rearm() {
    ArmImages();
    ArmTime();
}

bool PromotionPlaylist::ShouldShowNow() {
    if (m_entries.empty() || !TriggersEnabled()) return false;

    bool due = false;

    // Image trigger — counts only real images, so a promotion never shortens
    // the gap to the next one.
    if (ImageTriggerEnabled() && --m_imgCountdown <= 0) {
        ArmImages();
        due = true;
    }

    // Time trigger — evaluated at slide boundaries, the only moment a promotion
    // can take over the screen. With a long slide interval the promo therefore
    // appears at the next change rather than interrupting mid-image.
    if (TimeTriggerEnabled() && GetTickCount64() >= m_nextDueTick) {
        ArmTime();
        due = true;
    }

    // Both coming due on the same image still yields ONE promotion; each
    // trigger re-armed itself above, so they stay independent.
    return due;
}

} // namespace Dedicated
