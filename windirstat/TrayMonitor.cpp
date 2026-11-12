// CyberDirStat - tray icon that warns when a drive runs low on free space
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "TrayMonitor.h"
#include "DiskHistory.h"

#include <shellapi.h>

// Behaviour:
//  - enabled from Tools -> Free space monitor; off by default
//  - every 5 minutes checks fixed drives; once an hour records a point for the fill-up chart
//  - a drive under the threshold raises one notification, repeated at most every 6 hours
//    while it stays low; going back above the threshold re-arms it
//  - while enabled, the title-bar close button hides the window to the tray;
//    File -> Exit and the tray menu's Exit still quit
//  - "Start with Windows" adds an HKCU Run entry that launches "<exe>" /tray

namespace
{
    constexpr ULONGLONG oneMinute = 60ULL * 10'000'000;
    constexpr ULONGLONG historyInterval = 55 * oneMinute;
    constexpr ULONGLONG realertInterval = 6 * 60 * oneMinute;
    constexpr auto runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto runValue = L"CyberDirStat";

    enum TrayCommand : UINT { CmdOpen = 1, CmdHistory, CmdCheck, CmdExit };

    bool s_iconAdded = false;
    bool s_hiddenHintShown = false;
    ULONGLONG s_lastHistory = 0;
    std::unordered_map<std::wstring, ULONGLONG> s_lastAlert;

    ULONGLONG Now()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    NOTIFYICONDATAW BaseData(const HWND frame)
    {
        NOTIFYICONDATAW data{ .cbSize = sizeof(NOTIFYICONDATAW), .hWnd = frame, .uID = 1 };
        return data;
    }

    void AddIcon(const HWND frame)
    {
        if (s_iconAdded) return;
        NOTIFYICONDATAW data = BaseData(frame);
        data.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        data.uCallbackMessage = TrayMonitor::WM_TRAYICON;
        data.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_MAINFRAME), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        wcsncpy_s(data.szTip, Localization::Lookup(IDS_TRAY_TIP).c_str(), _TRUNCATE);
        s_iconAdded = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void RemoveIcon(const HWND frame)
    {
        if (!s_iconAdded) return;
        NOTIFYICONDATAW data = BaseData(frame);
        Shell_NotifyIconW(NIM_DELETE, &data);
        s_iconAdded = false;
    }

    void Notify(const HWND frame, const std::wstring& title, const std::wstring& text, const DWORD icon)
    {
        if (!s_iconAdded) return;
        NOTIFYICONDATAW data = BaseData(frame);
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = icon;
        wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    std::wstring Percent(const ULONGLONG part, const ULONGLONG total)
    {
        return std::format(L"{}%", total == 0 ? 0 : static_cast<int>(part * 100 / total));
    }

    // Checks every fixed drive. "manual" also reports when everything is fine.
    void Check(const HWND frame, const bool manual)
    {
        const ULONGLONG now = Now();
        const bool recordHistory = now - s_lastHistory >= historyInterval;
        if (recordHistory) s_lastHistory = now;

        std::vector<std::wstring> lowLines, allLines;
        bool alert = false;

        std::array<wchar_t, 512> drives{};
        const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
        for (const wchar_t* drive = drives.data(); length > 0 && length < drives.size() && *drive != L'\0'; drive += wcslen(drive) + 1)
        {
            if (GetDriveTypeW(drive) != DRIVE_FIXED) continue;
            ULONGLONG used = 0, total = 0;
            if (!DiskHistory::QueryVolume(drive, used, total)) continue;
            const ULONGLONG free = total - used;

            if (recordHistory) DiskHistory::Append(drive, used, total);

            allLines.push_back(Localization::Format(IDS_TRAY_OK_LINEss, drive, FormatBytes(free)));
            if (!TrayMonitor::IsLow(free, total, COptions::LowSpacePercent))
            {
                s_lastAlert.erase(drive); // re-arm once there is room again
                continue;
            }

            lowLines.push_back(Localization::Format(IDS_TRAY_LOW_LINEssss, drive, FormatBytes(free), FormatBytes(total), Percent(free, total)));
            if (const auto it = s_lastAlert.find(drive); it == s_lastAlert.end() || now - it->second >= realertInterval)
            {
                s_lastAlert[drive] = now;
                alert = true;
            }
        }

        const auto join = [](const std::vector<std::wstring>& lines)
        {
            std::wstring text;
            for (const auto& line : lines) text += (text.empty() ? L"" : L"\n") + line;
            return text;
        };

        if (!lowLines.empty() && (alert || manual))
            Notify(frame, Localization::Lookup(IDS_TRAY_LOW_TITLE), join(lowLines), NIIF_WARNING);
        else if (manual)
            Notify(frame, Localization::Lookup(IDS_TRAY_OK_TITLE), join(allLines), NIIF_INFO);
    }

    void ShowMenu(const HWND frame)
    {
        const HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return;
        AppendMenuW(menu, MF_STRING, CmdOpen, Localization::Lookup(IDS_TRAY_OPEN).c_str());
        AppendMenuW(menu, MF_STRING, CmdHistory, Localization::Lookup(IDS_MENU_HISTORY).c_str());
        AppendMenuW(menu, MF_STRING, CmdCheck, Localization::Lookup(IDS_TRAY_CHECK).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, CmdExit, Localization::Lookup(IDS_MENU_EXIT).c_str());
        SetMenuDefaultItem(menu, CmdOpen, FALSE);

        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(frame); // required so the menu closes when clicking elsewhere
        const auto command = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            cursor.x, cursor.y, 0, frame, nullptr));
        PostMessageW(frame, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (command)
        {
        case CmdOpen: TrayMonitor::RestoreWindow(frame); break;
        case CmdHistory:
            TrayMonitor::RestoreWindow(frame);
            PostMessageW(frame, WM_COMMAND, ID_TOOLS_HISTORY, 0);
            break;
        case CmdCheck: Check(frame, true); break;
        case CmdExit: PostMessageW(frame, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }

    std::wstring AutostartCommand()
    {
        return L"\"" + GetAppFileName() + L"\" /tray";
    }
}

bool TrayMonitor::IsLow(const ULONGLONG free, const ULONGLONG total, const int percent)
{
    return total > 0 && free * 100 < total * static_cast<ULONGLONG>(std::max(percent, 0));
}

bool TrayMonitor::IsEnabled()
{
    return COptions::TrayMonitor;
}

void TrayMonitor::Start(const HWND frame)
{
    if (!IsEnabled()) return;
    AddIcon(frame);
    SetTimer(frame, TimerId, CheckIntervalMs, nullptr);
    if (IsAutostartEnabled()) SetAutostart(true); // keep the Run entry pointing at this exe
    Check(frame, false);
}

void TrayMonitor::SetEnabled(const HWND frame, const bool enabled)
{
    COptions::TrayMonitor = enabled;
    if (enabled)
    {
        Start(frame);
        return;
    }
    KillTimer(frame, TimerId);
    RemoveIcon(frame);
    SetAutostart(false); // autostart only makes sense with monitoring
    s_lastAlert.clear();
}

void TrayMonitor::Shutdown()
{
    if (!s_iconAdded) return;
    RemoveIcon(GetMainWindowHandle());
}

void TrayMonitor::CheckNow(const HWND frame)
{
    Check(frame, true);
}

void TrayMonitor::OnTimer(const HWND frame)
{
    if (IsEnabled()) Check(frame, false);
}

void TrayMonitor::OnTrayMessage(const HWND frame, const LPARAM event)
{
    switch (static_cast<UINT>(event))
    {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        RestoreWindow(frame);
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowMenu(frame);
        break;
    case NIN_BALLOONUSERCLICK:
        RestoreWindow(frame);
        PostMessageW(frame, WM_COMMAND, ID_TOOLS_HISTORY, 0);
        break;
    default:
        break;
    }
}

bool TrayMonitor::HideToTray(const HWND frame)
{
    if (!IsEnabled() || !s_iconAdded) return false;
    ShowWindow(frame, SW_HIDE);
    if (!s_hiddenHintShown)
    {
        s_hiddenHintShown = true;
        Notify(frame, Localization::Lookup(IDS_TRAY_TIP), Localization::Lookup(IDS_TRAY_HIDDEN), NIIF_INFO);
    }
    return true;
}

void TrayMonitor::RestoreWindow(const HWND frame)
{
    ShowWindow(frame, IsIconic(frame) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(frame);
}

bool TrayMonitor::IsAutostartEnabled()
{
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, runKey, runValue, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

void TrayMonitor::SetAutostart(const bool enabled)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled)
    {
        const std::wstring command = AutostartCommand();
        RegSetValueExW(key, runValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(key, runValue);
    }
    RegCloseKey(key);
}
