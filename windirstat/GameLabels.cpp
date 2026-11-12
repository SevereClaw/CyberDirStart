// CyberDirStat - labels launcher folders (Steam, Epic Games) with game names
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GameLabels.h"
#include "SteamLibrary.h"
#include "Item.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <shared_mutex>

// What gets a label:
//  Steam (every library listed in libraryfolders.vdf), using appmanifest_<id>.acf names:
//    steamapps\workshop\content\<id>, shadercache\<id>, downloading\<id>, temp\<id>, compatdata\<id>,
//    Steam\userdata\<account>\<id>, and steamapps\common\<installdir> when the folder name differs.
//  Epic Games Launcher manifests (%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item):
//    the install folder when its name differs from the game's display name.

namespace
{
    namespace fs = std::filesystem;

    struct Label
    {
        std::wstring folderName; // guards against a reused CItem address
        std::wstring game;
    };

    std::shared_mutex s_mutex;
    std::unordered_map<const CItem*, Label> s_labels;
    std::atomic<bool> s_hasLabels = false;

    using SteamLibrary::ReadUtf8File;
    using SteamLibrary::NormalizePath;
}

std::vector<std::wstring> GameLabels::VdfValues(const std::wstring_view text, const std::wstring_view key)
{
    return SteamLibrary::VdfValues(text, key);
}

std::wstring GameLabels::VdfValue(const std::wstring_view text, const std::wstring_view key)
{
    return SteamLibrary::VdfValue(text, key);
}

std::wstring GameLabels::JsonString(const std::wstring_view json, const std::wstring_view key)
{
    const std::wstring quotedKey = L"\"" + std::wstring(key) + L"\"";
    const size_t keyPos = json.find(quotedKey);
    if (keyPos == std::wstring_view::npos) return {};
    size_t i = json.find_first_not_of(L" \t\r\n", keyPos + quotedKey.size());
    if (i == std::wstring_view::npos || json[i] != L':') return {};
    i = json.find_first_not_of(L" \t\r\n", i + 1);
    if (i == std::wstring_view::npos || json[i] != L'"') return {};

    std::wstring value;
    for (++i; i < json.size() && json[i] != L'"'; ++i)
    {
        if (json[i] != L'\\' || i + 1 >= json.size())
        {
            value.push_back(json[i]);
            continue;
        }
        const wchar_t next = json[++i];
        switch (next)
        {
        case L'n': value.push_back(L'\n'); break;
        case L't': value.push_back(L'\t'); break;
        case L'r': value.push_back(L'\r'); break;
        case L'b': value.push_back(L'\b'); break;
        case L'f': value.push_back(L'\f'); break;
        case L'u':
            if (i + 4 < json.size())
            {
                const std::wstring hex(json.substr(i + 1, 4));
                value.push_back(static_cast<wchar_t>(wcstoul(hex.c_str(), nullptr, 16)));
                i += 4;
            }
            break;
        default: value.push_back(next); break; // \\ \" \/
        }
    }
    return value;
}

bool GameLabels::SameName(const std::wstring_view folder, const std::wstring_view game)
{
    const auto normalize = [](const std::wstring_view s)
    {
        std::wstring result;
        for (const wchar_t c : s) if (std::iswalnum(c)) result.push_back(static_cast<wchar_t>(std::towlower(c)));
        return result;
    };
    const std::wstring a = normalize(folder), b = normalize(game);
    if (a.empty() || b.empty()) return true; // nothing useful to add
    return a == b || a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
}

namespace
{
    using Candidates = std::vector<std::pair<std::wstring, std::wstring>>; // path, game

    void CollectSteam(Candidates& candidates)
    {
        const SteamLibrary::Data steam = SteamLibrary::Load();
        if (steam.apps.empty()) return;

        std::unordered_map<std::wstring, const SteamLibrary::App*> byId;
        for (const auto& app : steam.apps) byId[app.id] = &app;

        // Folders named by app id (workshop, shader cache, downloads, saves)
        for (const auto& [path, id] : SteamLibrary::IdFolders(steam, true))
        {
            if (const auto app = byId.find(id); app != byId.end()) candidates.emplace_back(path, app->second->name);
        }

        // Install folders whose name differs from the game's name
        for (const auto& app : steam.apps)
        {
            if (app.installDir.empty() || GameLabels::SameName(app.installDir, app.name)) continue;
            candidates.emplace_back((fs::path(app.library) / L"steamapps" / L"common" / app.installDir).wstring(), app.name);
        }
    }

    void CollectEpic(Candidates& candidates)
    {
        std::array<wchar_t, MAX_PATH> programData{};
        const DWORD length = GetEnvironmentVariableW(L"ProgramData", programData.data(), static_cast<DWORD>(programData.size()));
        if (length == 0 || length >= programData.size()) return;

        std::error_code ec;
        const fs::path manifests = fs::path(programData.data()) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
        for (const auto& entry : fs::directory_iterator(manifests, ec))
        {
            if (entry.path().extension() != L".item") continue;
            const std::wstring json = ReadUtf8File(entry.path());
            const std::wstring name = GameLabels::JsonString(json, L"DisplayName");
            const std::wstring location = NormalizePath(GameLabels::JsonString(json, L"InstallLocation"));
            if (name.empty() || location.size() <= 3) continue;
            if (GameLabels::SameName(fs::path(location).filename().wstring(), name)) continue;
            candidates.emplace_back(location, name);
        }
    }
}

void GameLabels::Clear()
{
    std::unique_lock lock(s_mutex);
    s_labels.clear();
    s_hasLabels = false;
}

void GameLabels::Rebuild(const CItem* root)
{
    if (root == nullptr) return;

    Candidates candidates;
    CollectSteam(candidates);
    CollectEpic(candidates);

    std::unordered_map<const CItem*, Label> labels;
    CItem::ChildNameIndex index;
    for (const auto& [path, game] : candidates)
    {
        const CItem* item = root->FindItemByPath(path, false, IT_NONE, &index);
        if (item == nullptr || !item->IsTypeOrFlag(IT_DIRECTORY)) continue;
        labels[item] = { std::wstring(item->GetNameView()), game };
    }

    std::unique_lock lock(s_mutex);
    s_labels = std::move(labels);
    s_hasLabels = !s_labels.empty();
}

std::wstring GameLabels::Lookup(const CItem* item)
{
    if (!s_hasLabels) return {};
    std::shared_lock lock(s_mutex);
    const auto it = s_labels.find(item);
    if (it == s_labels.end() || it->second.folderName != item->GetNameView()) return {};
    return it->second.game;
}
