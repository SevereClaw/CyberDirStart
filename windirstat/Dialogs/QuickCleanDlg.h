// CyberDirStat - quick cleanup of well-known caches and temporary files
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"
#include "Layout.h"

//
// CQuickCleanDlg. Lists predefined cleanup categories (temp files, browser,
// shader and developer caches, ...) with their current size and deletes the
// contents of the selected ones after confirmation.
//
class CQuickCleanDlg final : public MessageTarget<CQuickCleanDlg, CLayoutDialog>
{
public:
    explicit CQuickCleanDlg(CWnd* pParent = nullptr);
    ~CQuickCleanDlg() override = default;

    enum { IDD = IDD_QUICKCLEAN };

    // One cleanup category: folders whose *contents* may be removed.
    struct Category
    {
        std::wstring_view nameId;              // localization key
        std::vector<std::wstring_view> roots;  // %ENV% paths; a "*" component matches any subfolder
        bool needsAdmin = false;               // usually only writable when elevated
        bool olderThanDay = false;             // keep files modified during the last 24 hours
        bool checkedByDefault = true;
    };

    // Totals for one category.
    struct Stats
    {
        ULONGLONG bytes = 0;
        ULONGLONG files = 0;
        ULONGLONG skipped = 0;
    };

    static const std::vector<Category>& GetCategories();
    static std::vector<std::wstring> ResolveRoots(const Category& category);
    static Stats Process(const Category& category, bool remove);

protected:
    bool OnInitDialog() override;

public:
    static std::span<const RouteEntry> Routes();

protected:
    void OnClean();
    void OnItemToggled();
    void Measure();
    void UpdateTotal();

    std::vector<Stats> m_stats;
};

inline std::span<const RouteEntry> CQuickCleanDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnClean>(BN_CLICKED, IDOK),
        Route::Control<&OnItemToggled>(BN_CLICKED, IDC_QUICKCLEAN_ITEM0, IDC_QUICKCLEAN_ITEM6),
    };
    return entries;
}
