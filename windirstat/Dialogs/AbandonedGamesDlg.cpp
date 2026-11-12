// CyberDirStat - Steam games that have not been played for a long time
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "AbandonedGamesDlg.h"

// Nothing is deleted here: "Uninstall via Steam" opens steam://uninstall/<id>,
// so Steam asks for confirmation and removes the game the proper way.

namespace
{
    constexpr std::array periodMonths{ 3, 6, 12 };
    constexpr int defaultPeriod = 1; // 6 months

    enum Column { ColName, ColSize, ColLastPlayed, ColLibrary, ColCount };

    ULONGLONG NowUnix()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return t / 10'000'000ULL - 11'644'473'600ULL;
    }

    std::wstring UnixDate(const ULONGLONG unix)
    {
        const ULONGLONG t = (unix + 11'644'473'600ULL) * 10'000'000ULL;
        const FILETIME utc{ .dwLowDateTime = static_cast<DWORD>(t & 0xFFFFFFFFULL), .dwHighDateTime = static_cast<DWORD>(t >> 32) };
        FILETIME local{};
        SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};
        std::array<wchar_t, 64> buffer{};
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, buffer.data(),
            static_cast<int>(buffer.size()), nullptr) == 0) return {};
        return buffer.data();
    }
}

std::vector<SteamLibrary::App> CAbandonedGamesDlg::SelectAbandoned(const std::vector<SteamLibrary::App>& apps, const ULONGLONG cutoff)
{
    std::vector<SteamLibrary::App> result;
    for (const auto& app : apps)
    {
        const bool stale = app.lastPlayed != 0
            ? app.lastPlayed < cutoff
            : app.lastUpdated != 0 && app.lastUpdated < cutoff; // never launched, installed long ago
        if (stale) result.push_back(app);
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.sizeOnDisk > b.sizeOnDisk; });
    return result;
}

CAbandonedGamesDlg::CAbandonedGamesDlg(CWnd* pParent)
    : MessageTarget(IDD_ABANDONED, nullptr, pParent)
{
}

bool CAbandonedGamesDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);

    m_period.SubclassDlgItem(IDC_ABANDONED_PERIOD, this);
    m_list.SubclassDlgItem(IDC_ABANDONED_LIST, this);

    for (const auto id : { IDS_ABANDONED_3M, IDS_ABANDONED_6M, IDS_ABANDONED_12M }) m_period.AddString(Localization::Lookup(id));
    m_period.SetCurSel(defaultPeriod);

    m_list.SetExtendedStyle(m_list.GetExtendedStyle() | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
    const int width = m_list.GetClientRect().Width();
    m_list.InsertColumn(ColName, Localization::Lookup(IDS_ABANDONED_COL_GAME), LVCFMT_LEFT, width * 40 / 100);
    m_list.InsertColumn(ColSize, Localization::Lookup(IDS_COL_SIZE_PHYSICAL), LVCFMT_RIGHT, width * 15 / 100);
    m_list.InsertColumn(ColLastPlayed, Localization::Lookup(IDS_ABANDONED_COL_PLAYED), LVCFMT_LEFT, width * 20 / 100);
    m_list.InsertColumn(ColLibrary, Localization::Lookup(IDS_ABANDONED_COL_LIBRARY), LVCFMT_LEFT, width * 25 / 100 - 4);

    m_layout.AddControl(IDC_ABANDONED_LIST, 0, 0, 1, 1);
    m_layout.AddControl(IDC_ABANDONED_TOTAL, 0, 1, 1, 0);
    m_layout.AddControl(IDC_ABANDONED_UNINSTALL, 1, 1, 0, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);

    DarkMode::AdjustControls(Handle());
    Reload();
    return true;
}

void CAbandonedGamesDlg::Reload()
{
    CWaitCursor wait;
    const SteamLibrary::Data steam = SteamLibrary::Load();
    const int period = std::clamp(m_period.GetCurSel(), 0, static_cast<int>(periodMonths.size()) - 1);
    const ULONGLONG cutoff = NowUnix() - static_cast<ULONGLONG>(periodMonths[static_cast<size_t>(period)]) * 30 * 24 * 60 * 60;
    m_games = SelectAbandoned(steam.apps, cutoff);

    m_list.SetItemCountEx(static_cast<int>(m_games.size()), 0);
    m_list.Invalidate();

    ULONGLONG total = 0;
    for (const auto& game : m_games) total += game.sizeOnDisk;
    if (steam.steamPath.empty()) SetText(IDC_ABANDONED_TOTAL, Localization::Lookup(IDS_ABANDONED_NO_STEAM));
    else if (m_games.empty()) SetText(IDC_ABANDONED_TOTAL, Localization::Lookup(IDS_ABANDONED_NONE));
    else SetText(IDC_ABANDONED_TOTAL, Localization::Format(IDS_ABANDONED_TOTALss, std::to_wstring(m_games.size()), FormatBytes(total)));
    UpdateButtons();
}

int CAbandonedGamesDlg::SelectedIndex() const
{
    const int index = m_list.GetNextItem(-1, LVNI_SELECTED);
    return index >= 0 && static_cast<size_t>(index) < m_games.size() ? index : -1;
}

void CAbandonedGamesDlg::UpdateButtons()
{
    GetDlgItem(IDC_ABANDONED_UNINSTALL).EnableWindow(SelectedIndex() >= 0);
}

void CAbandonedGamesDlg::OnUninstall()
{
    const int index = SelectedIndex();
    if (index < 0) return;
    const std::wstring url = L"steam://uninstall/" + m_games[static_cast<size_t>(index)].id;
    ShellExecuteW(Handle(), L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CAbandonedGamesDlg::OnDisplayInfo(NMHDR* header, LRESULT* result)
{
    *result = 0;
    auto* info = reinterpret_cast<NMLVDISPINFOW*>(header);
    const int item = info->item.iItem;
    if ((info->item.mask & LVIF_TEXT) == 0 || item < 0 || static_cast<size_t>(item) >= m_games.size() ||
        info->item.pszText == nullptr || info->item.cchTextMax <= 0) return;

    const auto& game = m_games[static_cast<size_t>(item)];
    std::wstring text;
    switch (info->item.iSubItem)
    {
    case ColName: text = game.name; break;
    case ColSize: text = FormatBytes(game.sizeOnDisk); break;
    case ColLastPlayed: text = game.lastPlayed == 0 ? Localization::Lookup(IDS_ABANDONED_NEVER) : UnixDate(game.lastPlayed); break;
    case ColLibrary: text = game.library; break;
    default: break;
    }
    wcsncpy_s(info->item.pszText, static_cast<size_t>(info->item.cchTextMax), text.c_str(), _TRUNCATE);
}

void CAbandonedGamesDlg::OnCustomDraw(NMHDR* header, LRESULT* result)
{
    *result = CDRF_DODEFAULT;
    if (!DarkMode::IsDarkModeActive()) return;

    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
    {
        *result = CDRF_NOTIFYITEMDRAW;
    }
    else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
    {
        draw->clrText = DarkMode::SystemColor(COLOR_WINDOWTEXT);
        draw->clrTextBk = DarkMode::SystemColor(COLOR_WINDOW);
    }
}
