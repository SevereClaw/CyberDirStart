// CyberDirStat - checks GitHub for a newer release and installs it
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

// Release version baked in by the CI release build ("v1.1"); empty for development builds.
#include "CyberVersion.h"

namespace UpdateChecker
{
    // Message posted to the main frame by background work; lParam owns a Result*.
    inline constexpr UINT WM_UPDATECHECK = WM_APP + 0x200;

    struct Result
    {
        enum class Stage { Checked, Downloaded, Failed };

        Stage stage = Stage::Checked;
        bool manual = false;       // user asked explicitly: report "up to date" and errors too
        bool ok = false;           // request and parsing succeeded
        std::wstring latestTag;    // e.g. "v1.2"
        std::wstring assetUrl;     // download URL of the exe matching this build (x64/x86)
        std::wstring assetSha256;  // lowercase hex SHA-256 published by GitHub for the asset
        UINT errorId = 0;          // localized reason when stage == Failed
    };

    // Version of this build ("v1.1"), or empty for development builds.
    std::wstring CurrentVersion();

    // Release asset name for this build's architecture.
    std::wstring AssetName();

    // Starts a background check; the result arrives as WM_UPDATECHECK at the main window.
    void Start(bool manual);

    // Handles a posted result on the main thread. Takes ownership of the result.
    void OnResult(LPARAM lParam);

    // Removes leftovers of a previous self-update (<exe>.old, <exe>.update); call at startup.
    void CleanupAfterUpdate();

    // Compares "v1.10" style versions: negative if a < b, 0 if equal, positive if a > b.
    int CompareVersions(std::wstring_view a, std::wstring_view b);

    // --- helpers exposed for tests ---
    std::wstring ExtractString(std::string_view json, std::string_view field);
    bool ParseAsset(std::string_view json, std::wstring_view assetName, std::wstring& url, std::wstring& sha256);
}
