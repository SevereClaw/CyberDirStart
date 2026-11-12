// CyberDirStat - Steam games that have not been played for a long time
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"
#include "Layout.h"
#include "SteamLibrary.h"

class CAbandonedGamesDlg final : public MessageTarget<CAbandonedGamesDlg, CLayoutDialog>
{
public:
    explicit CAbandonedGamesDlg(CWnd* pParent = nullptr);
    ~CAbandonedGamesDlg() override = default;

    enum { IDD = IDD_ABANDONED };

    // Installed games not launched since the cutoff (never-launched games count when installed
    // or updated before it), largest first. Times are Unix seconds.
    static std::vector<SteamLibrary::App> SelectAbandoned(const std::vector<SteamLibrary::App>& apps, ULONGLONG cutoff);

protected:
    bool OnInitDialog() override;

public:
    static std::span<const RouteEntry> Routes();

protected:
    void Reload();
    void OnPeriodChanged() { Reload(); }
    void OnUninstall();
    void UpdateButtons();
    void OnSelectionChanged(NMHDR*, LRESULT* result) { *result = 0; UpdateButtons(); }
    void OnDisplayInfo(NMHDR* header, LRESULT* result);
    void OnCustomDraw(NMHDR* header, LRESULT* result);
    int SelectedIndex() const;

    CComboBox m_period;
    CListCtrl m_list;
    std::vector<SteamLibrary::App> m_games;
};

inline std::span<const RouteEntry> CAbandonedGamesDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnPeriodChanged>(CBN_SELCHANGE, IDC_ABANDONED_PERIOD),
        Route::Control<&OnUninstall>(BN_CLICKED, IDC_ABANDONED_UNINSTALL),
        Route::Notify<&OnSelectionChanged>(LVN_ITEMCHANGED, IDC_ABANDONED_LIST),
        Route::Notify<&OnSelectionChanged>(LVN_ODSTATECHANGED, IDC_ABANDONED_LIST),
        Route::Notify<&OnDisplayInfo>(LVN_GETDISPINFO, IDC_ABANDONED_LIST),
        Route::Notify<&OnCustomDraw>(NM_CUSTOMDRAW, IDC_ABANDONED_LIST),
    };
    return entries;
}
