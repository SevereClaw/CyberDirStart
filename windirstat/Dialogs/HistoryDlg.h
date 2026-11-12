// CyberDirStat - chart of how a drive/folder filled up over time
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"
#include "Layout.h"
#include "DiskHistory.h"

class CHistoryDlg final : public MessageTarget<CHistoryDlg, CLayoutDialog>
{
public:
    explicit CHistoryDlg(CWnd* pParent = nullptr);
    ~CHistoryDlg() override = default;

    enum { IDD = IDD_HISTORY };

    // Owner-drawn line chart: used space over time, plus capacity for drives.
    class Chart final : public CStatic
    {
    public:
        std::vector<DiskHistory::Point> points;
        void DrawItem(LPDRAWITEMSTRUCT item) override;
    };

    // Summary sentence for a series: current state, trend per day and a fill-up forecast.
    static std::wstring Summarize(const std::vector<DiskHistory::Point>& points);

protected:
    bool OnInitDialog() override;

public:
    static std::span<const RouteEntry> Routes();

protected:
    void OnRootChanged();

    Chart m_chart;
    CComboBox m_roots;
    std::vector<std::wstring> m_rootPaths;
};

inline std::span<const RouteEntry> CHistoryDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnRootChanged>(CBN_SELCHANGE, IDC_HISTORY_ROOT),
    };
    return entries;
}
