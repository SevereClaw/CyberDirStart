// CyberDirStat - labels launcher folders (Steam, Epic Games) with game names
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GameLabels.h"
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

    std::wstring ReadUtf8File(const fs::path& file)
    {
        std::error_code ec;
        if (fs::file_size(file, ec) > 16 * 1024 * 1024 || ec) return {};
        std::ifstream in(file, std::ios::binary);
        if (!in) return {};
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (bytes.starts_with("\xEF\xBB\xBF")) bytes.erase(0, 3);
        if (bytes.empty()) return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring text(static_cast<size_t>(std::max(length, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), length);
        return text;
    }

    std::wstring RegistryString(const HKEY root, const wchar_t* key, const wchar_t* value)
    {
        DWORD size = 0;
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) return {};
        std::wstring result(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, result.data(), &size) != ERROR_SUCCESS) return {};
        result.resize(wcsnlen(result.c_str(), result.size()));
        return result;
    }

    std::wstring NormalizePath(std::wstring path)
    {
        std::ranges::replace(path, L'/', L'\\');
        while (path.size() > 3 && path.ends_with(L'\\')) path.pop_back();
        return path;
    }

    std::vector<fs::path> Subfolders(const fs::path& folder)
    {
        std::vector<fs::path> result;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec))
        {
            if (entry.is_directory(ec) && !entry.is_symlink(ec)) result.push_back(entry.path());
        }
        return result;
    }

    struct Token
    {
        bool isString;
        std::wstring text; // string contents, or "{" / "}"
    };

    std::vector<Token> TokenizeVdf(const std::wstring_view text)
    {
        std::vector<Token> tokens;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const wchar_t c = text[i];
            if (c == L'{' || c == L'}')
            {
                tokens.push_back({ false, std::wstring(1, c) });
            }
            else if (c == L'/' && i + 1 < text.size() && text[i + 1] == L'/')
            {
                while (i < text.size() && text[i] != L'\n') ++i; // comment
            }
            else if (c == L'"')
            {
                std::wstring value;
                for (++i; i < text.size() && text[i] != L'"'; ++i)
                {
                    if (text[i] == L'\\' && i + 1 < text.size())
                    {
                        const wchar_t next = text[++i];
                        value.push_back(next == L'n' ? L'\n' : next == L't' ? L'\t' : next);
                    }
                    else
                    {
                        value.push_back(text[i]);
                    }
                }
                tokens.push_back({ true, std::move(value) });
            }
        }
        return tokens;
    }

    struct SteamApp
    {
        std::wstring name;
        std::wstring installDir;
    };
}

std::vector<std::wstring> GameLabels::VdfValues(const std::wstring_view text, const std::wstring_view key)
{
    std::vector<std::wstring> values;
    const auto tokens = TokenizeVdf(text);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        if (tokens[i].isString && tokens[i + 1].isString && _wcsicmp(tokens[i].text.c_str(), std::wstring(key).c_str()) == 0)
        {
            values.push_back(tokens[i + 1].text);
            ++i;
        }
    }
    return values;
}

std::wstring GameLabels::VdfValue(const std::wstring_view text, const std::wstring_view key)
{
    auto values = VdfValues(text, key);
    return values.empty() ? std::wstring{} : std::move(values.front());
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
        std::wstring steam = NormalizePath(RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
        if (steam.empty()) steam = NormalizePath(RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
        if (steam.empty()) return;

        // Libraries: the Steam folder itself plus every "path" in libraryfolders.vdf
        std::vector<std::wstring> libraries{ steam };
        for (const auto* vdf : { L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf" })
        {
            for (auto& path : GameLabels::VdfValues(ReadUtf8File(fs::path(steam) / vdf), L"path"))
            {
                path = NormalizePath(path);
                if (!path.empty() && std::ranges::none_of(libraries, [&](const auto& l) { return _wcsicmp(l.c_str(), path.c_str()) == 0; }))
                    libraries.push_back(path);
            }
        }

        // App names from the manifests of every library
        std::unordered_map<std::wstring, SteamApp> apps;
        std::vector<std::pair<std::wstring, std::wstring>> installs; // library, appid
        for (const auto& library : libraries)
        {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(fs::path(library) / L"steamapps", ec))
            {
                const std::wstring file = entry.path().filename().wstring();
                if (!file.starts_with(L"appmanifest_") || !file.ends_with(L".acf")) continue;
                const std::wstring text = ReadUtf8File(entry.path());
                const std::wstring id = GameLabels::VdfValue(text, L"appid");
                const std::wstring name = GameLabels::VdfValue(text, L"name");
                if (id.empty() || name.empty()) continue;
                apps[id] = { name, GameLabels::VdfValue(text, L"installdir") };
                installs.emplace_back(library, id);
            }
        }
        if (apps.empty()) return;

        for (const auto& library : libraries)
        {
            const fs::path steamapps = fs::path(library) / L"steamapps";
            for (const auto* sub : { L"workshop\\content", L"shadercache", L"downloading", L"temp", L"compatdata" })
            {
                for (const auto& folder : Subfolders(steamapps / sub))
                {
                    if (const auto app = apps.find(folder.filename().wstring()); app != apps.end())
                        candidates.emplace_back(folder.wstring(), app->second.name);
                }
            }
        }

        for (const auto& account : Subfolders(fs::path(steam) / L"userdata"))
        {
            for (const auto& folder : Subfolders(account))
            {
                if (const auto app = apps.find(folder.filename().wstring()); app != apps.end())
                    candidates.emplace_back(folder.wstring(), app->second.name);
            }
        }

        for (const auto& [library, id] : installs)
        {
            const SteamApp& app = apps[id];
            if (app.installDir.empty() || GameLabels::SameName(app.installDir, app.name)) continue;
            candidates.emplace_back((fs::path(library) / L"steamapps" / L"common" / app.installDir).wstring(), app.name);
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
