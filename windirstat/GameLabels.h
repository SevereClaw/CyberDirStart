// CyberDirStat - labels launcher folders (Steam, Epic Games) with game names
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

class CItem;

namespace GameLabels
{
    // Forget all labels (call before the scanned tree changes).
    void Clear();

    // Reads launcher metadata and labels matching folders of the scanned tree.
    // Called by the scanning thread once a scan has finished.
    void Rebuild(const CItem* root);

    // Game name for a folder, or empty. Cheap; called while painting.
    std::wstring Lookup(const CItem* item);

    // --- metadata parsing (exposed for tests) ---

    // Value of the first "key" "value" pair in Valve KeyValues text (ACF/VDF), unescaped.
    std::wstring VdfValue(std::wstring_view text, std::wstring_view key);
    // All values of "key" in Valve KeyValues text, unescaped.
    std::vector<std::wstring> VdfValues(std::wstring_view text, std::wstring_view key);
    // Value of a top-level JSON string field, unescaped (\\, \", \/, \uXXXX).
    std::wstring JsonString(std::wstring_view json, std::wstring_view key);
    // True when a folder name already says the same as the game name ("Half-Life 2" vs "half life 2").
    bool SameName(std::wstring_view folder, std::wstring_view game);
}
