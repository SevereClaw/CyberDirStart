// CyberDirStat - checks GitHub for a newer release
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

namespace
{
    constexpr auto apiHost = L"api.github.com";
    constexpr auto apiPath = L"/repos/SevereClaw/CyberDirStart/releases/latest";
    constexpr auto releasesPage = L"https://github.com/SevereClaw/CyberDirStart/releases/latest";

    // Downloads the "latest release" JSON. Returns an empty string on any failure.
    std::string FetchLatestReleaseJson()
    {
        const SmartPointer session(WinHttpCloseHandle, WinHttpOpen(L"CyberDirStat-UpdateCheck",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (session == nullptr) return {};
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);

        const SmartPointer connection(WinHttpCloseHandle, WinHttpConnect(session, apiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (connection == nullptr) return {};

        const SmartPointer request(WinHttpCloseHandle, WinHttpOpenRequest(connection, L"GET", apiPath, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (request == nullptr) return {};

        constexpr auto headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) return {};

        DWORD status = 0, size = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return {};

        std::string body;
        constexpr size_t maxBody = 4 * 1024 * 1024;
        for (DWORD available = 0; WinHttpQueryDataAvailable(request, &available) && available > 0;)
        {
            const size_t offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + offset, available, &read)) return {};
            body.resize(offset + read);
            if (read == 0 || body.size() > maxBody) break;
        }
        return body;
    }

    // Extracts a top-level string field such as "tag_name" from the JSON text.
    std::wstring ExtractString(const std::string& json, const std::string_view field)
    {
        const std::string key = "\"" + std::string(field) + "\"";
        const size_t keyPos = json.find(key);
        if (keyPos == std::string::npos) return {};
        const size_t colon = json.find(':', keyPos + key.size());
        if (colon == std::string::npos) return {};
        const size_t open = json.find('"', colon + 1);
        if (open == std::string::npos) return {};
        const size_t close = json.find('"', open + 1);
        if (close == std::string::npos || close - open > 64) return {};

        std::wstring result;
        for (const char c : std::string_view(json).substr(open + 1, close - open - 1))
        {
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return {};
            result.push_back(static_cast<wchar_t>(c));
        }
        return result;
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
}

std::wstring UpdateChecker::CurrentVersion()
{
    return CYBERDIRSTAT_VERSION;
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

void UpdateChecker::Start(const bool manual)
{
    const HWND target = GetMainWindowHandle();
    std::thread([target, manual]
    {
        auto result = std::make_unique<Result>();
        result->manual = manual;
        result->latestTag = ExtractString(FetchLatestReleaseJson(), "tag_name");
        result->ok = !result->latestTag.empty();

        // PostMessage to a closed window simply fails; then the result is freed here
        if (PostMessageW(target, WM_UPDATECHECK, 0, reinterpret_cast<LPARAM>(result.get()))) static_cast<void>(result.release());
    }).detach();
}

void UpdateChecker::OnResult(const LPARAM lParam)
{
    const std::unique_ptr<Result> result(reinterpret_cast<Result*>(lParam));
    if (result == nullptr) return;

    const std::wstring current = CurrentVersion();
    const std::wstring title = Localization::Lookup(IDS_UPDATE_TITLE);

    if (!result->ok)
    {
        if (result->manual) ShowMessageBox(GetMainWindowHandle(), Localization::Lookup(IDS_UPDATE_FAILED),
            title, MB_OK | MB_ICONWARNING);
        return;
    }

    if (current.empty())
    {
        // Development build: no version to compare against
        if (result->manual) ShowMessageBox(GetMainWindowHandle(),
            Localization::Format(IDS_UPDATE_DEVs, result->latestTag), title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (CompareVersions(result->latestTag, current) <= 0)
    {
        if (result->manual) ShowMessageBox(GetMainWindowHandle(),
            Localization::Format(IDS_UPDATE_LATESTs, current), title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Automatic checks respect "don't remind me about this version"
    if (!result->manual && COptions::SkippedUpdate.Obj() == result->latestTag) return;

    const auto [answer, skip] = CMessageBoxDlg::Show(
        Localization::Format(IDS_UPDATE_AVAILABLEss, result->latestTag, current),
        result->manual ? std::wstring{} : Localization::Lookup(IDS_UPDATE_SKIP), false,
        MB_YESNO | MB_ICONINFORMATION, {}, {}, title);

    if (skip) COptions::SkippedUpdate = result->latestTag;
    if (answer == IDYES)
    {
        ShellExecuteW(GetMainWindowHandle(), L"open", releasesPage, nullptr, nullptr, SW_SHOWNORMAL);
    }
}
