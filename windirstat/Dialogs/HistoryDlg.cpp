// CyberDirStat - chart of how a drive/folder filled up over time
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "HistoryDlg.h"
#include "GrowthTracker.h"

namespace
{
    constexpr double oneDay = 24.0 * 60 * 60 * 10'000'000; // FILETIME units

    std::wstring ShortDate(const ULONGLONG time)
    {
        const FILETIME utc{ .dwLowDateTime = static_cast<DWORD>(time & 0xFFFFFFFFULL), .dwHighDateTime = static_cast<DWORD>(time >> 32) };
        FILETIME local{};
        SYSTEMTIME st{};
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return {};
        std::array<wchar_t, 64> buffer{};
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, buffer.data(),
            static_cast<int>(buffer.size()), nullptr) == 0) return {};
        return buffer.data();
    }

    std::wstring SignedBytes(const double bytes)
    {
        const ULONGLONG magnitude = static_cast<ULONGLONG>(std::llround(std::abs(bytes)));
        return (bytes < 0 ? L"−" : L"+") + FormatBytes(magnitude);
    }

    Gdiplus::Color ToColor(const COLORREF c, const BYTE alpha = 255)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    }
}

std::wstring CHistoryDlg::Summarize(const std::vector<DiskHistory::Point>& points)
{
    if (points.empty()) return Localization::Lookup(IDS_HISTORY_EMPTY);

    const auto& last = points.back();
    std::wstring text;
    if (last.total > 0)
    {
        const ULONGLONG free = last.total - std::min(last.used, last.total);
        text = Localization::Format(IDS_HISTORY_DRIVE_STATUSsss, FormatBytes(last.used), FormatBytes(last.total), FormatBytes(free));
    }
    else
    {
        text = Localization::Format(IDS_HISTORY_FOLDER_STATUSs, FormatBytes(last.used));
    }

    const auto& first = points.front();
    const double days = static_cast<double>(last.time - first.time) / oneDay;
    if (points.size() < 2 || days < 1.0) return text + L" " + Localization::Lookup(IDS_HISTORY_NEED_TIME);

    const double delta = static_cast<double>(last.used) - static_cast<double>(first.used);
    const double perDay = delta / days;
    text += L" " + Localization::Format(IDS_HISTORY_TRENDsss, ShortDate(first.time), SignedBytes(delta), SignedBytes(perDay));

    // Linear forecast, only when the drive is actually filling up
    if (last.total > 0 && perDay > 1024.0 * 1024)
    {
        const double free = static_cast<double>(last.total - std::min(last.used, last.total));
        if (const double daysLeft = free / perDay; daysLeft < 3650)
            text += L" " + Localization::Format(IDS_HISTORY_FORECASTs, std::to_wstring(std::max<long long>(1, std::llround(daysLeft))));
    }
    return text;
}

void CHistoryDlg::Chart::DrawItem(const LPDRAWITEMSTRUCT item)
{
    auto target = CDC::Borrow(item->hDC);
    CBufferedDC dc(target, this);
    const CRect rc = GetClientRect();

    const bool neon = DarkMode::IsCyberpunkActive();
    const COLORREF background = DarkMode::SystemColor(COLOR_WINDOW);
    const COLORREF text = DarkMode::SystemColor(COLOR_WINDOWTEXT);
    const COLORREF line = neon ? DarkMode::NeonCyan : RGB(0, 120, 215);
    const COLORREF capacity = neon ? DarkMode::NeonMagenta : RGB(200, 60, 60);
    const COLORREF grid = CColorSpace::BlendColor(text, background, 0.85);

    dc.FillSolidRect(rc, background);

    Gdiplus::Graphics g(dc.Handle());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    auto hFont = reinterpret_cast<HFONT>(SendMessage(WM_GETFONT));
    if (hFont == nullptr) hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const Gdiplus::Font font(dc.Handle(), hFont);
    const Gdiplus::SolidBrush textBrush(ToColor(text));

    if (points.empty())
    {
        Gdiplus::StringFormat center;
        center.SetAlignment(Gdiplus::StringAlignmentCenter);
        center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        const std::wstring message = Localization::Lookup(IDS_HISTORY_EMPTY);
        const Gdiplus::RectF box(0, 0, static_cast<Gdiplus::REAL>(rc.Width()), static_cast<Gdiplus::REAL>(rc.Height()));
        g.DrawString(message.c_str(), -1, &font, box, &center, &textBrush);
        return;
    }

    // Value range: capacity if known, otherwise a little headroom over the largest value
    ULONGLONG maxValue = 1;
    for (const auto& p : points) maxValue = std::max({ maxValue, p.used, p.total });
    const bool hasCapacity = points.back().total > 0;
    const double top = static_cast<double>(maxValue) * (hasCapacity ? 1.02 : 1.10);

    // Plot area with room for labels
    Gdiplus::RectF measure;
    g.MeasureString(L"000.0 GiB", -1, &font, Gdiplus::PointF(0, 0), &measure);
    const Gdiplus::REAL left = measure.Width + 8, right = static_cast<Gdiplus::REAL>(rc.Width()) - 12;
    const Gdiplus::REAL plotTop = measure.Height, bottom = static_cast<Gdiplus::REAL>(rc.Height()) - measure.Height - 6;
    if (right - left < 40 || bottom - plotTop < 30) return;

    const ULONGLONG t0 = points.front().time, t1 = points.back().time;
    const auto x = [&](const ULONGLONG t)
    {
        if (t1 == t0) return (left + right) / 2;
        return left + (right - left) * static_cast<Gdiplus::REAL>(static_cast<double>(t - t0) / static_cast<double>(t1 - t0));
    };
    const auto y = [&](const ULONGLONG v) { return bottom - (bottom - plotTop) * static_cast<Gdiplus::REAL>(static_cast<double>(v) / top); };

    // Horizontal grid with size labels
    const Gdiplus::Pen gridPen(ToColor(grid), 1.0f);
    Gdiplus::StringFormat rightAligned;
    rightAligned.SetAlignment(Gdiplus::StringAlignmentFar);
    rightAligned.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    // Drives: quarters of the capacity, so the top label is the drive size
    const double gridMax = hasCapacity ? static_cast<double>(points.back().total) : top;
    for (int i = 0; i <= 4; ++i)
    {
        const auto value = static_cast<ULONGLONG>(gridMax * i / 4);
        const Gdiplus::REAL gy = y(value);
        g.DrawLine(&gridPen, left, gy, right, gy);
        const std::wstring label = FormatBytes(value);
        g.DrawString(label.c_str(), -1, &font, Gdiplus::RectF(0, gy - measure.Height / 2, left - 6, measure.Height), &rightAligned, &textBrush);
    }

    // Date labels: first, middle, last
    Gdiplus::StringFormat centered;
    centered.SetAlignment(Gdiplus::StringAlignmentCenter);
    const std::array<ULONGLONG, 3> ticks{ t0, t0 + (t1 - t0) / 2, t1 };
    for (size_t i = 0; i < (t1 == t0 ? 1u : ticks.size()); ++i)
    {
        const std::wstring label = ShortDate(ticks[i]);
        const Gdiplus::REAL tx = std::clamp(x(ticks[i]), left + measure.Width / 2, right - measure.Width / 2);
        g.DrawString(label.c_str(), -1, &font, Gdiplus::RectF(tx - measure.Width, bottom + 4, measure.Width * 2, measure.Height), &centered, &textBrush);
    }

    // Capacity line (drives)
    if (hasCapacity)
    {
        Gdiplus::Pen capPen(ToColor(capacity), 1.5f);
        capPen.SetDashStyle(Gdiplus::DashStyleDash);
        std::vector<Gdiplus::PointF> cap;
        for (const auto& p : points) cap.emplace_back(x(p.time), y(p.total));
        if (cap.size() == 1) cap = { Gdiplus::PointF(left, cap.front().Y), Gdiplus::PointF(right, cap.front().Y) };
        g.DrawLines(&capPen, cap.data(), static_cast<INT>(cap.size()));
    }

    // Used space: filled area, optional neon glow, crisp line, last point marker
    std::vector<Gdiplus::PointF> used;
    for (const auto& p : points) used.emplace_back(x(p.time), y(p.used));
    if (used.size() == 1) used = { Gdiplus::PointF(left, used.front().Y), Gdiplus::PointF(right, used.front().Y) };

    std::vector<Gdiplus::PointF> area = used;
    area.emplace_back(used.back().X, bottom);
    area.emplace_back(used.front().X, bottom);
    const Gdiplus::SolidBrush areaBrush(ToColor(line, neon ? 40 : 55));
    g.FillPolygon(&areaBrush, area.data(), static_cast<INT>(area.size()));

    if (neon)
    {
        Gdiplus::Pen glow(ToColor(line, 60), 7.0f);
        glow.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawLines(&glow, used.data(), static_cast<INT>(used.size()));
    }
    Gdiplus::Pen linePen(ToColor(line), 2.0f);
    linePen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawLines(&linePen, used.data(), static_cast<INT>(used.size()));

    const Gdiplus::SolidBrush marker(ToColor(neon ? DarkMode::NeonYellow : line));
    const Gdiplus::PointF lastPoint(x(points.back().time), y(points.back().used));
    g.FillEllipse(&marker, lastPoint.X - 4, lastPoint.Y - 4, 8.0f, 8.0f);
}

CHistoryDlg::CHistoryDlg(CWnd* pParent)
    : MessageTarget(IDD_HISTORY, nullptr, pParent)
{
}

bool CHistoryDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);

    m_chart.SubclassDlgItem(IDC_HISTORY_CHART, this);
    m_roots.SubclassDlgItem(IDC_HISTORY_ROOT, this);

    m_layout.AddControl(IDC_HISTORY_ROOT, 0, 0, 0, 0);
    m_layout.AddControl(IDC_HISTORY_CHART, 0, 0, 1, 1);
    m_layout.AddControl(IDC_HISTORY_SUMMARY, 0, 1, 1, 0);
    m_layout.AddControl(IDC_HISTORY_FOLDERS, 0, 1, 1, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);

    // Roots with history; prefer the one scanned last
    m_rootPaths = DiskHistory::Roots();
    int selection = 0;
    const auto reports = GrowthTracker::GetReports();
    for (const auto& [i, root] : std::views::enumerate(m_rootPaths))
    {
        m_roots.AddString(root);
        if (!reports.empty() && _wcsicmp(root.c_str(), reports.front().root.c_str()) == 0) selection = static_cast<int>(i);
    }
    if (!m_rootPaths.empty()) m_roots.SetCurSel(selection);
    m_roots.EnableWindow(m_rootPaths.size() > 1);

    DarkMode::AdjustControls(Handle());
    OnRootChanged();
    return true;
}

void CHistoryDlg::OnRootChanged()
{
    const int index = m_roots.GetCurSel();
    if (index < 0 || static_cast<size_t>(index) >= m_rootPaths.size())
    {
        m_chart.points.clear();
        SetText(IDC_HISTORY_SUMMARY, Localization::Lookup(IDS_HISTORY_EMPTY));
        SetText(IDC_HISTORY_FOLDERS, L"");
        m_chart.Invalidate();
        return;
    }

    const std::wstring& root = m_rootPaths[static_cast<size_t>(index)];
    m_chart.points = DiskHistory::Load(root);
    SetText(IDC_HISTORY_SUMMARY, root + L": " + Summarize(m_chart.points));

    const auto growth = GrowthTracker::LongTermGrowth(root);
    std::wstring folders;
    if (!growth.hasBaseline)
    {
        folders = Localization::Lookup(IDS_HISTORY_FOLDERS_NEED_SCANS);
    }
    else if (growth.grown.empty())
    {
        folders = Localization::Format(IDS_HISTORY_FOLDERS_SINCEs, FormatFileTime(growth.baselineTime)) + L"\r\n" +
            Localization::Format(IDS_GROWTH_NOCHANGEs, FormatBytes(GrowthTracker::threshold));
    }
    else
    {
        folders = Localization::Format(IDS_HISTORY_FOLDERS_SINCEs, FormatFileTime(growth.baselineTime));
        for (const auto& change : growth.grown) folders += L"\r\n" + GrowthTracker::FormatChange(change);
    }
    SetText(IDC_HISTORY_FOLDERS, folders);
    m_chart.Invalidate();
}
