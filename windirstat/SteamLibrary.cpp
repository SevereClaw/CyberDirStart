// CyberDirStat - reads local Steam metadata (libraries, installed games, per-game folders)
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "SteamLibrary.h"

#include <fstream>

namespace
{
    namespace fs = std::filesystem;

    // Steam keeps per-game data in these steamapps subfolders, one folder per app id
    constexpr std::array idSubfolders{ L"workshop\\content", L"shadercache", L"downloading", L"temp", L"compatdata" };

    std::wstring RegistryString(const HKEY root, const wchar_t* key, const wchar_t* value)
    {
        DWORD size = 0;
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) return {};
        std::wstring result(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, result.data(), &size) != ERROR_SUCCESS) return {};
        result.resize(wcsnlen(result.c_str(), result.size()));
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

    bool IsAppId(const std::wstring_view s)
    {
        return !s.empty() && s.size() <= 12 && std::ranges::all_of(s, [](const wchar_t c) { return c >= L'0' && c <= L'9'; });
    }

    bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
}

std::wstring SteamLibrary::ReadUtf8File(const fs::path& file)
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

std::wstring SteamLibrary::NormalizePath(std::wstring path)
{
    std::ranges::replace(path, L'/', L'\\');
    while (path.size() > 3 && path.ends_with(L'\\')) path.pop_back();
    return path;
}

std::vector<fs::path> SteamLibrary::Subfolders(const fs::path& folder)
{
    std::vector<fs::path> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, fs::directory_options::skip_permission_denied, ec))
    {
        if (entry.is_directory(ec) && !entry.is_symlink(ec)) result.push_back(entry.path());
    }
    return result;
}

std::vector<std::wstring> SteamLibrary::VdfValues(const std::wstring_view text, const std::wstring_view key)
{
    std::vector<std::wstring> values;
    const auto tokens = TokenizeVdf(text);
    const std::wstring wanted(key);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        if (tokens[i].isString && tokens[i + 1].isString && _wcsicmp(tokens[i].text.c_str(), wanted.c_str()) == 0)
        {
            values.push_back(tokens[i + 1].text);
            ++i;
        }
    }
    return values;
}

std::wstring SteamLibrary::VdfValue(const std::wstring_view text, const std::wstring_view key)
{
    auto values = VdfValues(text, key);
    return values.empty() ? std::wstring{} : std::move(values.front());
}

std::vector<std::wstring> SteamLibrary::VdfBlockKeys(const std::wstring_view text, const std::wstring_view blockName)
{
    std::vector<std::wstring> keys;
    const auto tokens = TokenizeVdf(text);
    const std::wstring wanted(blockName);
    for (size_t i = 0; i + 1 < tokens.size(); ++i)
    {
        if (!tokens[i].isString || _wcsicmp(tokens[i].text.c_str(), wanted.c_str()) != 0 ||
            tokens[i + 1].isString || tokens[i + 1].text != L"{") continue;

        // Direct children: "key" "value" pairs (nested blocks are skipped)
        int depth = 0;
        bool expectKey = true;
        for (size_t j = i + 2; j < tokens.size(); ++j)
        {
            const Token& t = tokens[j];
            if (!t.isString)
            {
                if (t.text == L"{") { ++depth; expectKey = true; continue; }
                if (depth == 0) { i = j; break; }
                --depth;
                expectKey = true;
                continue;
            }
            if (depth > 0) continue;
            if (expectKey) keys.push_back(t.text);
            expectKey = !expectKey;
        }
    }
    return keys;
}

SteamLibrary::Data SteamLibrary::LoadFrom(const std::wstring& steamPath)
{
    Data data;
    data.steamPath = NormalizePath(steamPath);
    if (data.steamPath.empty()) return data;

    // Libraries: the Steam folder itself plus every "path" in libraryfolders.vdf.
    // Each library also lists its installed app ids - kept even for offline drives.
    data.libraries.push_back(data.steamPath);
    for (const auto* vdf : { L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf" })
    {
        const std::wstring text = ReadUtf8File(fs::path(data.steamPath) / vdf);
        for (auto& path : VdfValues(text, L"path"))
        {
            path = NormalizePath(path);
            if (!path.empty() && std::ranges::none_of(data.libraries, [&](const auto& l) { return SamePath(l, path); }))
                data.libraries.push_back(path);
        }
        for (auto& id : VdfBlockKeys(text, L"apps")) if (IsAppId(id)) data.knownIds.insert(std::move(id));
    }

    for (const auto& library : data.libraries)
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::path(library) / L"steamapps", ec))
        {
            const std::wstring file = entry.path().filename().wstring();
            if (!file.starts_with(L"appmanifest_") || !file.ends_with(L".acf")) continue;
            const std::wstring text = ReadUtf8File(entry.path());
            App app{
                .id = VdfValue(text, L"appid"),
                .name = VdfValue(text, L"name"),
                .installDir = VdfValue(text, L"installdir"),
                .library = library,
                .lastPlayed = _wcstoui64(VdfValue(text, L"LastPlayed").c_str(), nullptr, 10),
                .lastUpdated = _wcstoui64(VdfValue(text, L"LastUpdated").c_str(), nullptr, 10),
                .sizeOnDisk = _wcstoui64(VdfValue(text, L"SizeOnDisk").c_str(), nullptr, 10) };

            // A manifest file name carries the id too: trust it even if the content is damaged
            const std::wstring fileId = file.substr(12, file.size() - 12 - 4);
            if (IsAppId(fileId)) data.knownIds.insert(fileId);
            if (!IsAppId(app.id) || app.name.empty()) continue;
            data.knownIds.insert(app.id);
            data.apps.push_back(std::move(app));
        }
    }
    return data;
}

SteamLibrary::Data SteamLibrary::Load()
{
    std::wstring steam = RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    return steam.empty() ? Data{} : LoadFrom(steam);
}

std::vector<std::pair<std::wstring, std::wstring>> SteamLibrary::IdFolders(const Data& data, const bool includeUserData)
{
    std::vector<std::pair<std::wstring, std::wstring>> result;
    for (const auto& library : data.libraries)
    {
        const fs::path steamapps = fs::path(library) / L"steamapps";
        for (const auto* sub : idSubfolders)
        {
            for (const auto& folder : Subfolders(steamapps / sub))
            {
                const std::wstring id = folder.filename().wstring();
                if (IsAppId(id)) result.emplace_back(folder.wstring(), id);
            }
        }
    }

    if (includeUserData && !data.steamPath.empty())
    {
        for (const auto& account : Subfolders(fs::path(data.steamPath) / L"userdata"))
        {
            for (const auto& folder : Subfolders(account))
            {
                const std::wstring id = folder.filename().wstring();
                if (IsAppId(id)) result.emplace_back(folder.wstring(), id);
            }
        }
    }
    return result;
}

std::vector<std::wstring> SteamLibrary::FindLeftovers(const Data& data)
{
    // Without Steam's own list of installed games there is no safe way to tell what is left over
    if (data.knownIds.empty()) return {};

    std::vector<std::wstring> leftovers;
    for (auto& [path, id] : IdFolders(data, false)) // saves in userdata are never touched
    {
        if (!data.knownIds.contains(id)) leftovers.push_back(std::move(path));
    }
    return leftovers;
}
