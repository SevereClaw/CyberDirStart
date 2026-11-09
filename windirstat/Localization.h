// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

struct string_hash {
    using is_transparent = void;
    size_t operator()(const std::wstring_view txt) const { return std::hash<std::wstring_view>{}(txt); }
};

class Localization final
{
    static bool m_rightToLeft;
    static const std::wstring& GetResourceData();
    static bool CrackStrings(std::wstring_view data);
    static void SearchReplace(std::wstring& input, const std::wstring_view& search, const std::wstring_view& replace);
    static void UpdateWindowText(WindowRef wnd);
    static std::wstring FormatImpl(std::wstring_view format, std::wformat_args args);

public:
    static std::unordered_map<std::wstring, std::wstring, string_hash, std::equal_to<>> m_map;

    static bool Contains(const std::wstring_view name)
    {
        assert(m_map.contains(name));
        return m_map.contains(name);
    }

    static const std::wstring& Lookup(const std::wstring_view name)
    {
        static const std::wstring empty;
        const auto it = m_map.find(name);
        return it != m_map.end() ? it->second : empty;
    }

    static std::wstring LookupNeutral(const UINT res)
    {
        return LoadResourceString(res, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
    }

    template <typename... Args>
    static std::wstring Format(const std::wstring_view format, const Args&... args)
    {
        return FormatImpl(format, std::make_wformat_args(args...));
    }

    static bool IsRightToLeft() noexcept { return m_rightToLeft; }
    static void UpdateMenu(MenuRef menu);
    static void UpdateTabControl(CTabControl& tab);
    static void UpdateDialogs(WindowRef wnd);
    static bool LoadExternalLanguage(LCTYPE lcttype, LCID lcid);
    static bool LoadFile(const std::wstring& file);
    static bool LoadResource(LANGID language);

    // CyberDirStat: Russian with cyberpunk slang ships as lang_ru-MD.txt. It borrows the
    // ru-MD locale slot only to get its own LANGID; it is shown as "Russian (Cyberpunk)",
    // loads regular Russian as the fallback and is never picked automatically.
    static constexpr LANGID CyberpunkLanguage = 0x0819; // MAKELANGID(LANG_RUSSIAN, SUBLANG_RUSSIAN_MOLDAVIA)
    static std::wstring ConvertToWideString(const std::string_view& sv);
    static std::set<LANGID> GetLanguageList();
};
