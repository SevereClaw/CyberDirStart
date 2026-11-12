// CyberDirStat - reads local Steam metadata (libraries, installed games, per-game folders)
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

#include <filesystem>
#include <unordered_set>

namespace SteamLibrary
{
    struct App
    {
        std::wstring id;          // "1091500"
        std::wstring name;        // "Cyberpunk 2077"
        std::wstring installDir;  // folder name under steamapps\common
        std::wstring library;     // library root, e.g. "D:\SteamLibrary"
        ULONGLONG lastPlayed = 0; // Unix time; 0 = never launched
        ULONGLONG lastUpdated = 0;// Unix time of the last install/update
        ULONGLONG sizeOnDisk = 0; // bytes, as reported by Steam
    };

    struct Data
    {
        std::wstring steamPath;                  // Steam install folder
        std::vector<std::wstring> libraries;     // all library roots known to Steam (may be offline)
        std::vector<App> apps;                   // installed games with a manifest
        std::unordered_set<std::wstring> knownIds; // manifests + every library's "apps" list
    };

    // Reads the Steam installation registered for the current user (empty Data if none).
    Data Load();

    // Same, for a given Steam folder (tests).
    Data LoadFrom(const std::wstring& steamPath);

    // Folders named by app id: steamapps\{workshop\content,shadercache,downloading,temp,compatdata}\<id>
    // in every library, plus Steam\userdata\<account>\<id>. Pairs of (path, id).
    std::vector<std::pair<std::wstring, std::wstring>> IdFolders(const Data& data, bool includeUserData);

    // Cache/mod/download folders of games that are not installed in any library (never userdata).
    // Empty when Steam's own records could not be read, so nothing is ever guessed.
    std::vector<std::wstring> FindLeftovers(const Data& data);

    // --- text helpers shared with GameLabels ---
    std::wstring ReadUtf8File(const std::filesystem::path& file);
    std::wstring NormalizePath(std::wstring path);
    std::vector<std::filesystem::path> Subfolders(const std::filesystem::path& folder);
    std::vector<std::wstring> VdfValues(std::wstring_view text, std::wstring_view key);
    std::wstring VdfValue(std::wstring_view text, std::wstring_view key);
    // Keys inside every block named blockName, e.g. app ids in "apps" { "228980" "123" ... }.
    std::vector<std::wstring> VdfBlockKeys(std::wstring_view text, std::wstring_view blockName);
}
