// CyberDirStat - "What grew": compares folder sizes with an earlier scan
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

class CItem;

namespace GrowthTracker
{
    struct Change
    {
        std::wstring path;
        ULONGLONG before = 0;  // 0 = did not exist (or was tiny) in the earlier scan
        ULONGLONG after = 0;   // 0 = no longer exists
    };

    struct Report
    {
        std::wstring root;            // scanned drive or folder
        bool hasBaseline = false;     // false on the very first scan of this root
        FILETIME baselineTime{};      // when the compared scan was taken
        ULONGLONG before = 0, after = 0;
        std::vector<Change> grown;    // largest growth first
        std::vector<Change> shrunk;   // largest shrink first
    };

    // Called by the scanning thread after a completed scan: compares every scanned
    // drive/folder with its saved snapshot, then stores a fresh snapshot.
    void OnScanFinished(const CItem* root);

    // "+12 GiB    C:\Users\me\Downloads    (3 GiB -> 15 GiB)"
    std::wstring FormatChange(const Change& change);

    // Oldest vs newest stored snapshot of a root (for the fill-up chart window).
    Report LongTermGrowth(const std::wstring& root);

    // Reports from the latest finished scan (thread-safe copy).
    std::vector<Report> GetReports();

    // Shows the reports to the user (main thread).
    void ShowReport();

    // Minimum change worth listing.
    inline constexpr ULONGLONG threshold = 32ULL * 1024 * 1024;
}
