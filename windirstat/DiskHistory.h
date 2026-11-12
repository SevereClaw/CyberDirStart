// CyberDirStat - history of used space per drive/folder (for the fill-up chart)
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

#include <filesystem>

namespace DiskHistory
{
    struct Point
    {
        ULONGLONG time = 0;   // FILETIME (UTC, 100 ns)
        ULONGLONG used = 0;   // bytes in use
        ULONGLONG total = 0;  // drive capacity; 0 for a folder
    };

    // %LOCALAPPDATA%\CyberDirStat\Snapshots (shared with GrowthTracker); empty if unavailable.
    std::filesystem::path StorageFolder();

    // Stable file-name key for a drive/folder path (case-insensitive).
    std::wstring RootKey(const std::wstring& root);

    // Records the current state. A point less than 30 minutes after the previous one replaces it,
    // so hourly monitoring and repeated scans don't flood the file. Thread-safe.
    void Append(const std::wstring& root, ULONGLONG used, ULONGLONG total);

    // Points for one root, oldest first.
    std::vector<Point> Load(const std::wstring& root);

    // Roots that have a history, in the order drives/folders sort naturally.
    std::vector<std::wstring> Roots();

    // Used/total bytes of the volume holding the path; false if not a local volume.
    bool QueryVolume(const std::wstring& root, ULONGLONG& used, ULONGLONG& total);

    // --- exposed for tests ---
    std::wstring Serialize(const std::wstring& root, const std::vector<Point>& points);
    bool Parse(const std::wstring& text, std::wstring& root, std::vector<Point>& points);
    void AddPoint(std::vector<Point>& points, const Point& point);
}
