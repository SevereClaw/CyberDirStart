// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "PacMan.h"

namespace
{
    constexpr ULONGLONG HIDE_THRESHOLD = 750; // ms
    constexpr float MOUTHSPEED = 0.003f;      // aperture alteration / ms
    constexpr float PACMANSPEED = 0.09f;      // pixels / ms
}

void CPacman::Reset()
{
    m_lastUpdate   = 0;
    m_lastDraw     = 0;
    m_toTheRight   = true;
    m_position     = 0.0f;
    m_mouthOpening = true;
    m_aperture     = 0.0f;
    m_done         = false;
    m_moving       = false;
}

void CPacman::UpdatePosition()
{
    m_lastUpdate = GetTickCount64();
    if (m_lastDraw == 0) m_lastDraw = m_lastUpdate;
    m_moving = true;
    m_done = false;
}

void CPacman::Draw(CDC* pdc, const CRect& rect, const COLORREF backColor,
    const bool dots, const std::optional<bool> suspended)
{
    const ULONGLONG now = GetTickCount64();
    if (rect.Width() <= 0 || rect.Height() <= 0)
    {
        m_lastDraw = now;
        return;
    }
    if (suspended.value_or(m_suspended))
    {
        // Rebase time if suspended
        m_lastUpdate = now;
        m_lastDraw = now;
    }

    // See if we should still consider ourselves moving
    if (now - m_lastUpdate > HIDE_THRESHOLD) m_moving = false;

    // Update position
    if (m_moving)
    {
        const float delta = static_cast<float>(now - m_lastDraw);
        UpdatePosition(m_position, m_toTheRight, PACMANSPEED * delta / static_cast<float>(rect.Width()));
        UpdatePosition(m_aperture, m_mouthOpening, MOUTHSPEED * delta);
    }

    // Record time for next draw comparison
    m_lastDraw = now;

    // Calculate rectangle to display graphic
    CRect rc(rect);
    rc.Deflate(5, 1);
    const int size = std::min(rc.Width(), rc.Height()) / 2 * 2;
    rc.top += (rc.Height() - size) / 2;
    rc.bottom = rc.top + size;
    rc.left += static_cast<int>(m_position * (rc.Width() - size));
    rc.right = rc.left + size;
    const Gdiplus::Rect grect(rc.left, rc.top, rc.Width(), rc.Height());

    // Determine the share of the figure
    const float slice = m_aperture * 90.0f;
    const Gdiplus::REAL sweepAngle = 360.0f - slice;
    Gdiplus::REAL startAngle = m_aperture * slice / 2.0f;
    if (!m_toTheRight) startAngle += 180.0f;

    // Fill background if requested
    if (backColor != ~COLORREF())
    {
        pdc->FillSolidRect(rect, backColor);
    }
    if (m_done || size <= 0) return;

    // CyberDirStat: neon "scanner" instead of Pac-Man in the Cyberpunk theme
    if (DarkMode::IsCyberpunkActive())
    {
        DrawNeonScanner(pdc, rect, rc, size, dots, now);
        return;
    }

    // Create pens and brushes
    static const Gdiplus::Pen blackPen(Gdiplus::Color(0xFF, 0x00, 0x00, 0x00), 1);
    static const Gdiplus::SolidBrush yellowBrush(Gdiplus::Color(0xFF, 0xFC, 0xC9, 0x2F));

    // Draw filled shape if we started and recently updated
    Gdiplus::Graphics graphics(pdc->Handle());
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (dots)
    {
        const int spacing = std::max(8, rc.Height()), diameter = std::max(2, rc.Height() / 5);
        for (int x = rect.left + spacing; x < rect.right - spacing / 2; x += spacing)
        {
            if (m_toTheRight ? x <= rc.right : x >= rc.left) continue;
            graphics.FillEllipse(&yellowBrush, x - diameter / 2,
                rc.top + (rc.Height() - diameter) / 2, diameter, diameter);
        }
    }
    graphics.FillPie(&yellowBrush, grect, startAngle, sweepAngle);
    graphics.DrawPie(&blackPen, grect, startAngle, sweepAngle);
    if (m_moving) return;

    // Draw sleepy graphic
    const COLORREF zColor = DarkMode::IsDarkModeActive() ? 0x888888 : 0x000000;
    static int cachedFontSizePercent = 0;
    static std::optional<Gdiplus::Font> font;
    const int fontSizePercent = GetFontSizePercent();
    if (!font || cachedFontSizePercent != fontSizePercent)
    {
        font.emplace(wds::strFontArial, 6.0f * fontSizePercent / 100.0f, Gdiplus::FontStyleBold);
        cachedFontSizePercent = fontSizePercent;
    }
    const Gdiplus::SolidBrush zBrush(Gdiplus::Color(0xFF, GetRValue(zColor), GetGValue(zColor), GetBValue(zColor)));
    for (const auto x : { 1.0f, 2.0f, 3.0f })
    {
        const auto deltaX = x * static_cast<float>(rc.Width()) / 3.0f;
        const auto deltaY = (x + 1) * static_cast<float>(rc.Height()) / 18.0f;
        graphics.DrawString(L"z", 1, &*font, { rc.left + deltaX, rc.top - deltaY }, &zBrush);
    }
}

void CPacman::DrawNeonScanner(CDC* pdc, const CRect& rect, const CRect& rc, const int size,
    const bool dots, const ULONGLONG now) const
{
    Gdiplus::Graphics graphics(pdc->Handle());
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const auto argb = [](const BYTE alpha, const COLORREF c)
    {
        return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
    };

    const int headW = std::max(2, size / 3);
    const int headX = rc.left + (size - headW) / 2;

    // Idle: blinking terminal cursor
    if (!m_moving)
    {
        if ((now / 500) % 2 == 0)
        {
            const int cursorH = std::max(2, size / 4);
            const Gdiplus::SolidBrush cursor(argb(0xFF, DarkMode::NeonCyan));
            graphics.FillRectangle(&cursor, headX, rc.bottom - cursorH, headW, cursorH);
        }
        return;
    }

    // Flickering "data bits" ahead of the scanner
    if (dots)
    {
        const int spacing = std::max(8, rc.Height()), bit = std::max(2, rc.Height() / 5);
        for (int x = rect.left + spacing; x < rect.right - spacing / 2; x += spacing)
        {
            if (m_toTheRight ? x <= rc.right : x >= rc.left) continue;
            const auto hash = static_cast<std::uint32_t>(x * 2654435761u ^ static_cast<std::uint32_t>(now / 140) * 40503u);
            const BYTE alpha = static_cast<BYTE>(90 + (hash >> 16) % 166);
            const Gdiplus::SolidBrush brush(argb(alpha, DarkMode::NeonYellow));
            graphics.FillRectangle(&brush, x - bit / 2, rc.top + (rc.Height() - bit) / 2, bit, bit);
        }
    }

    // Fading trail behind the head: cyan -> magenta
    constexpr int trailSegments = 7;
    const int step = headW + std::max(1, headW / 2);
    const int direction = m_toTheRight ? -1 : 1;
    for (int i = trailSegments; i >= 1; --i)
    {
        const float t = static_cast<float>(i) / trailSegments;
        const COLORREF color = CColorSpace::BlendColor(DarkMode::NeonCyan, DarkMode::NeonMagenta, t);
        const BYTE alpha = static_cast<BYTE>(220 * (1.0f - t) + 20);
        const int segH = std::max(2, static_cast<int>(size * (1.0f - 0.5f * t)));
        const int x = headX + direction * step * i;
        if (x + headW < rect.left || x > rect.right) continue;
        const Gdiplus::SolidBrush brush(argb(alpha, color));
        graphics.FillRectangle(&brush, x, rc.top + (size - segH) / 2, headW, segH);
    }

    // Head: soft glow, cyan core, white-hot center line
    const int glow = std::max(1, size / 4);
    const Gdiplus::SolidBrush glowBrush(argb(0x55, DarkMode::NeonCyan));
    graphics.FillRectangle(&glowBrush, headX - glow, rc.top - glow / 2, headW + 2 * glow, size + glow);
    const Gdiplus::SolidBrush coreBrush(argb(0xFF, DarkMode::NeonCyan));
    graphics.FillRectangle(&coreBrush, headX, rc.top, headW, size);
    const Gdiplus::SolidBrush hotBrush(argb(0xFF, RGB(235, 255, 255)));
    graphics.FillRectangle(&hotBrush, headX + headW / 2 - std::max(1, headW / 6) / 2, rc.top + 1,
        std::max(1, headW / 6), std::max(1, size - 2));
}

void CPacman::UpdatePosition(float& position, bool& up, const float diff)
{
    assert(diff >= 0.0f);
    assert(position >= 0.0f);
    assert(position <= 1.0f);

    if (!up) position = 2.0f - position;
    position += diff;
    position = std::fmod(position, 2.0f);
    up = (position <= 1.0f);
    if (!up) position = 2.0f - position;
}
