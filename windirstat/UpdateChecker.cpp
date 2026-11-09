// CyberDirStat - checks GitHub for a newer release and installs it
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "UpdateChecker.h"
#include "MessageBoxDlg.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

// Self-update, all from the main window's point of view:
//  1. background: GET api.github.com/.../releases/latest -> tag, exe URL and its SHA-256 digest
//  2. user agrees -> background: download the exe, verify SHA-256 and the "MZ" header,
//     write it next to the running exe as <exe>.update
//  3. main thread: rename the running exe to <exe>.old (allowed while it runs),
//     rename <exe>.update to <exe>, offer to restart
//  4. the new instance deletes <exe>.old once the old process has exited

namespace
{
    constexpr auto apiUrl = L"https://api.github.com/repos/SevereClaw/CyberDirStart/releases/latest";
    constexpr auto releasesPage = L"https://github.com/SevereClaw/CyberDirStart/releases/latest";
    constexpr auto downloadPrefix = L"https://github.com/SevereClaw/CyberDirStart/releases/download/";

    std::wstring UpdatePath() { return GetAppFileName() + L".update"; }
    std::wstring OldPath() { return GetAppFileName() + L".old"; }

    // HTTPS GET (redirects followed). Returns false on any failure or a non-200 status.
    bool HttpGet(const std::wstring& url, const size_t maxBody, std::string& body)
    {
        URL_COMPONENTS parts{ .dwStructSize = sizeof(URL_COMPONENTS) };
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
            std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

        const SmartPointer session(WinHttpCloseHandle, WinHttpOpen(L"CyberDirStat-Update",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session == nullptr) return false;
        WinHttpSetTimeouts(session, 5000, 10000, 10000, 30000);

        const SmartPointer connection(WinHttpCloseHandle, WinHttpConnect(session, host.c_str(), parts.nPort, 0));
        if (connection == nullptr) return false;

        const SmartPointer request(WinHttpCloseHandle, WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request == nullptr) return false;

        constexpr auto headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) return false;

        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;

        body.clear();
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;)
        {
            const size_t offset = body.size();
            if (offset + available > maxBody) return false;
            body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + offset, available, &read)) return false;
            body.resize(offset + read);
            if (read == 0) break;
        }
        return !body.empty();
    }

    std::wstring Sha256Hex(const std::string& data)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
        std::array<UCHAR, 32> digest{};
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
        ok = ok && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
            static_cast<ULONG>(data.size()), 0) == 0;
        ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
        if (hash != nullptr) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok) return {};

        std::wstring hex;
        for (const UCHAR b : digest) hex += std::format(L"{:02x}", b);
        return hex;
    }

    std::vector<int> ParseVersion(std::wstring_view version)
    {
        std::vector<int> parts;
        int value = -1;
        for (const wchar_t c : version)
        {
            if (c >= L'0' && c <= L'9')
            {
                value = std::min((value < 0 ? 0 : value) * 10 + (c - L'0'), 1'000'000);
            }
            else if (value >= 0)
            {
                parts.push_back(value);
                value = -1;
            }
        }
        if (value >= 0) parts.push_back(value);
        return parts;
    }

    void Post(const HWND target, std::unique_ptr<UpdateChecker::Result> result)
    {
        // PostMessage to a closed window simply fails; then the result is freed here
        if (PostMessageW(target, UpdateChecker::WM_UPDATECHECK, 0, reinterpret_cast<LPARAM>(result.get())))
            static_cast<void>(result.release());
    }

    // Background: download, verify and stage the new exe as <exe>.update.
    void StartDownload(const UpdateChecker::Result& found)
    {
        const HWND target = GetMainWindowHandle();
        std::thread([target, found]
        {
            auto result = std::make_unique<UpdateChecker::Result>(found);
            result->stage = UpdateChecker::Result::Stage::Failed;

            std::string exe;
            if (!HttpGet(found.assetUrl, 128 * 1024 * 1024, exe))
            {
                result->errorId = 1;
            }
            else if (exe.size() < 2 || exe[0] != 'M' || exe[1] != 'Z' || Sha256Hex(exe) != found.assetSha256)
            {
                result->errorId = 2; // corrupted or tampered download
            }
            else
            {
                const std::wstring staging = UpdatePath();
                const SmartPointer file(CloseHandle, CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
                DWORD written = 0;
                if (file == INVALID_HANDLE_VALUE ||
                    !WriteFile(file, exe.data(), static_cast<DWORD>(exe.size()), &written, nullptr) ||
                    written != exe.size())
                {
                    result->errorId = 3; // no write access to the program folder
                }
                else
                {
                    result->stage = UpdateChecker::Result::Stage::Downloaded;
                }
            }
            if (result->stage == UpdateChecker::Result::Stage::Failed) DeleteFileW(UpdatePath().c_str());
            Post(target, std::move(result));
        }).detach();
    }

    // Main thread: put the staged exe in place of the running one.
    bool SwapExecutables()
    {
        const std::wstring exe = GetAppFileName(), staged = UpdatePath(), old = OldPath();
        DeleteFileW(old.c_str());
        if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
        if (!MoveFileExW(staged.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING); // roll back
            return false;
        }
        return true;
    }

    void ShowFailure(const UpdateChecker::Result& result)
    {
        const std::wstring reason = Localization::Lookup(
            result.errorId == 2 ? IDS_UPDATE_ERR_VERIFY : result.errorId == 3 ? IDS_UPDATE_ERR_WRITE : IDS_UPDATE_ERR_DOWNLOAD);
        if (ShowMessageBox(GetMainWindowHandle(), Localization::Format(IDS_UPDATE_INSTALL_FAILEDs, reason),
            Localization::Lookup(IDS_UPDATE_TITLE), MB_YESNO | MB_ICONWARNING) == IDYES)
        {
            ShellExecuteW(GetMainWindowHandle(), L"open", releasesPage, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
}

std::wstring UpdateChecker::ExtractString(const std::string_view json, const std::string_view field)
{
    const std::string key = "\"" + std::string(field) + "\"";
    const size_t keyPos = json.find(key);
    if (keyPos == std::string_view::npos) return {};
    const size_t colon = json.find(':', keyPos + key.size());
    if (colon == std::string_view::npos) return {};
    const size_t open = json.find('"', colon + 1);
    if (open == std::string_view::npos) return {};
    const size_t close = json.find('"', open + 1);
    if (close == std::string_view::npos || close - open > 1024) return {};

    std::wstring result;
    for (const char c : json.substr(open + 1, close - open - 1))
    {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return {};
        result.push_back(static_cast<wchar_t>(c));
    }
    return result;
}

bool UpdateChecker::ParseAsset(const std::string_view json, const std::wstring_view assetName, std::wstring& url, std::wstring& sha256)
{
    // The asset object starts with "name":"<asset>"; its digest and URL follow inside the same object
    std::string quoted = "\"";
    for (const wchar_t c : assetName) quoted.push_back(static_cast<char>(c & 0x7F)); // asset names are ASCII
    quoted.push_back('"');
    const size_t at = json.find(quoted);
    if (at == std::string_view::npos) return false;
    const std::string_view asset = json.substr(at, std::min<size_t>(json.size() - at, 8192));

    url = ExtractString(asset, "browser_download_url");
    std::wstring digest = ExtractString(asset, "digest");
    if (!url.starts_with(downloadPrefix) || !url.ends_with(L"/" + std::wstring(assetName))) return false;
    if (!digest.starts_with(L"sha256:") || digest.size() != 7 + 64) return false;
    sha256 = digest.substr(7);
    std::ranges::transform(sha256, sha256.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return std::ranges::all_of(sha256, [](const wchar_t c) { return std::iswxdigit(c) != 0; });
}

std::wstring UpdateChecker::CurrentVersion()
{
    return CYBERDIRSTAT_VERSION;
}

std::wstring UpdateChecker::AssetName()
{
#ifdef _WIN64
    return L"CyberDirStat-x64.exe";
#else
    return L"CyberDirStat-x86.exe";
#endif
}

int UpdateChecker::CompareVersions(const std::wstring_view a, const std::wstring_view b)
{
    const auto pa = ParseVersion(a), pb = ParseVersion(b);
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i)
    {
        const int x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

void UpdateChecker::CleanupAfterUpdate()
{
    const std::wstring old = OldPath(), staged = UpdatePath();
    if (GetFileAttributesW(old.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    // The previous instance may still be closing: retry for a little while
    std::thread([old, staged]
    {
        DeleteFileW(staged.c_str());
        for (int attempt = 0; attempt < 60; ++attempt)
        {
            if (DeleteFileW(old.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) break;
            Sleep(500);
        }
    }).detach();
}

void UpdateChecker::Start(const bool manual)
{
    const HWND target = GetMainWindowHandle();
    std::thread([target, manual]
    {
        auto result = std::make_unique<Result>();
        result->manual = manual;
        std::string json;
        if (HttpGet(apiUrl, 4 * 1024 * 1024, json))
        {
            result->latestTag = ExtractString(json, "tag_name");
            ParseAsset(json, AssetName(), result->assetUrl, result->assetSha256);
        }
        result->ok = !result->latestTag.empty();
        Post(target, std::move(result));
    }).detach();
}

void UpdateChecker::OnResult(const LPARAM lParam)
{
    const std::unique_ptr<Result> result(reinterpret_cast<Result*>(lParam));
    if (result == nullptr) return;

    const HWND window = GetMainWindowHandle();
    const std::wstring current = CurrentVersion();
    const std::wstring title = Localization::Lookup(IDS_UPDATE_TITLE);

    if (result->stage == Result::Stage::Failed)
    {
        ShowFailure(*result);
        return;
    }

    if (result->stage == Result::Stage::Downloaded)
    {
        if (!SwapExecutables())
        {
            DeleteFileW(UpdatePath().c_str());
            result->errorId = 3;
            ShowFailure(*result);
            return;
        }
        if (ShowMessageBox(window, Localization::Format(IDS_UPDATE_READYs, result->latestTag),
            title, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        {
            CDirStatApp::Get()->RestartApplication();
        }
        return;
    }

    if (!result->ok)
    {
        if (result->manual) ShowMessageBox(window, Localization::Lookup(IDS_UPDATE_FAILED), title, MB_OK | MB_ICONWARNING);
        return;
    }

    if (current.empty())
    {
        // Development build: no version to compare against
        if (result->manual) ShowMessageBox(window,
            Localization::Format(IDS_UPDATE_DEVs, result->latestTag), title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (CompareVersions(result->latestTag, current) <= 0)
    {
        if (result->manual) ShowMessageBox(window,
            Localization::Format(IDS_UPDATE_LATESTs, current), title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Automatic checks respect "don't remind me about this version"
    if (!result->manual && COptions::SkippedUpdate.Obj() == result->latestTag) return;

    // Without a verifiable download (should not happen for CI releases) fall back to the web page
    const bool canInstall = !result->assetUrl.empty() && !result->assetSha256.empty();
    const auto [answer, skip] = CMessageBoxDlg::Show(
        Localization::Format(canInstall ? IDS_UPDATE_INSTALLss : IDS_UPDATE_AVAILABLEss, result->latestTag, current),
        result->manual ? std::wstring{} : Localization::Lookup(IDS_UPDATE_SKIP), false,
        MB_YESNO | MB_ICONINFORMATION, {}, {}, title);

    if (skip) COptions::SkippedUpdate = result->latestTag;
    if (answer != IDYES) return;

    if (canInstall) StartDownload(*result);
    else ShellExecuteW(window, L"open", releasesPage, nullptr, nullptr, SW_SHOWNORMAL);
}
