// CyberDirStat - checks GitHub for a newer release
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
    // Message posted to the main frame when a check finishes; lParam owns a Result*.
    inline constexpr UINT WM_UPDATECHECK = WM_APP + 0x200;

    struct Result
    {
        bool manual = false;     // user asked explicitly: report "up to date" and errors too
        bool ok = false;         // request and parsing succeeded
        std::wstring latestTag;  // e.g. "v1.2"
    };

    // Version of this build ("v1.1"), or empty for development builds.
    std::wstring CurrentVersion();

    // Starts a background check; the result arrives as WM_UPDATECHECK at the main window.
    void Start(bool manual);

    // Shows the outcome to the user (main thread). Takes ownership of the posted result.
    void OnResult(LPARAM lParam);

    // Compares "v1.10" style versions: negative if a < b, 0 if equal, positive if a > b.
    int CompareVersions(std::wstring_view a, std::wstring_view b);
}
