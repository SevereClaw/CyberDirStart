// CyberDirStat - tray icon that warns when a drive runs low on free space
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

namespace TrayMonitor
{
    inline constexpr UINT WM_TRAYICON = WM_APP + 0x201;  // notify-icon callback to the main frame
    inline constexpr UINT_PTR TimerId = 0x5A11;           // main frame timer
    inline constexpr UINT CheckIntervalMs = 5 * 60 * 1000;

    bool IsEnabled();

    // Turns monitoring on/off for the running instance (icon + timer). Saves the option.
    void SetEnabled(HWND frame, bool enabled);

    // Starts monitoring when enabled (call once the main window exists).
    void Start(HWND frame);

    // Removes the icon (call when the main window is destroyed).
    void Shutdown();

    // Periodic check of fixed drives: records history and raises low-space notifications.
    void OnTimer(HWND frame);

    // Checks now and always reports the result (menu command).
    void CheckNow(HWND frame);

    // Notify-icon callback (clicks, menu, balloon click).
    void OnTrayMessage(HWND frame, LPARAM event);

    // Title-bar close: hide to tray instead of exiting when monitoring. True if handled.
    bool HideToTray(HWND frame);

    // Brings the main window back from the tray.
    void RestoreWindow(HWND frame);

    // "Start with Windows" (HKCU Run key, launches with /tray).
    bool IsAutostartEnabled();
    void SetAutostart(bool enabled);

    // Low-space rule: free share below the percentage threshold.
    bool IsLow(ULONGLONG free, ULONGLONG total, int percent);
}
