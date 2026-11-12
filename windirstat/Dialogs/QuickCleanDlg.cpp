// CyberDirStat - quick cleanup of well-known caches and temporary files
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "QuickCleanDlg.h"
#include "MessageBoxDlg.h"
#include "SteamLibrary.h"
#include "UiSounds.h"

// Safety rules for every category:
//  - only the *contents* of the listed folders are touched, never the folders themselves;
//  - reparse points (symlinks, junctions, cloud placeholders) are neither followed nor deleted;
//  - files that are in use or protected are skipped and counted;
//  - temp folders keep everything modified during the last 24 hours.

namespace
{
    constexpr int categoryCount = IDC_QUICKCLEAN_ITEM7 - IDC_QUICKCLEAN_ITEM0 + 1;

    // Size labels: SIZE0..SIZE6 are consecutive, SIZE7 was added later with its own id
    constexpr int SizeId(const int i) { return i < 7 ? IDC_QUICKCLEAN_SIZE0 + i : IDC_QUICKCLEAN_SIZE7; }

    std::vector<std::wstring> SteamLeftoverRoots()
    {
        return SteamLibrary::FindLeftovers(SteamLibrary::Load());
    }

    std::wstring LongPath(const std::wstring& path)
    {
        return path.starts_with(L"\\\\") ? path : L"\\\\?\\" + path;
    }

    bool IsPlainDirectory(const std::wstring& path)
    {
        const DWORD attributes = GetFileAttributesW(LongPath(path).c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }

    std::vector<std::wstring> ListSubfolders(const std::wstring& path)
    {
        std::vector<std::wstring> result;
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW(LongPath(path + L"\\*").c_str(), FindExInfoBasic, &data,
            FindExSearchLimitToDirectories, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return result;
        do
        {
            const std::wstring_view name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
            result.emplace_back(path + L"\\" + data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
        return result;
    }

    bool IsNewer(const FILETIME& time, const FILETIME* cutoff)
    {
        return cutoff != nullptr && CompareFileTime(&time, cutoff) > 0;
    }

    void Walk(const std::wstring& folder, const bool remove, const FILETIME* cutoff, CQuickCleanDlg::Stats& stats)
    {
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW(LongPath(folder + L"\\*").c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE) return;
        do
        {
            const std::wstring_view name = data.cFileName;
            if (name == L"." || name == L"..") continue;

            // Never follow or delete links: they may point anywhere
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;

            const std::wstring path = folder + L"\\" + data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                Walk(path, remove, cutoff, stats);

                // Remove the emptied subfolder (fails harmlessly while it still has content);
                // keep recently touched folders in temp locations, a running program may own them
                if (remove && !IsNewer(data.ftLastWriteTime, cutoff)) RemoveDirectoryW(LongPath(path).c_str());
                continue;
            }

            if (IsNewer(data.ftLastWriteTime, cutoff)) continue;

            const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (!remove)
            {
                stats.bytes += size;
                stats.files++;
                continue;
            }

            const std::wstring longPath = LongPath(path);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
            {
                SetFileAttributesW(longPath.c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            }
            if (DeleteFileW(longPath.c_str()))
            {
                stats.bytes += size;
                stats.files++;
            }
            else
            {
                stats.skipped++;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
}

const std::vector<CQuickCleanDlg::Category>& CQuickCleanDlg::GetCategories()
{
    // Chromium-based browsers keep caches per profile: <User Data>\<profile>\{Cache,Code Cache,GPUCache}
    static const std::vector<Category> categories
    {
        {
            .nameId = IDS_QUICKCLEAN_USER_TEMP,
            .roots = { L"%TEMP%" },
            .olderThanDay = true,
        },
        {
            .nameId = IDS_QUICKCLEAN_WINDOWS_TEMP,
            .roots = { L"%SystemRoot%\\Temp" },
            .needsAdmin = true,
            .olderThanDay = true,
        },
        {
            .nameId = IDS_QUICKCLEAN_CRASH_REPORTS,
            .roots = {
                L"%LOCALAPPDATA%\\CrashDumps",
                L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportArchive",
                L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportQueue",
                L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportArchive",
                L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportQueue",
            },
        },
        {
            .nameId = IDS_QUICKCLEAN_BROWSER,
            .roots = {
                L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Cache",
                L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Code Cache",
                L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\GPUCache",
                L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Cache",
                L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Code Cache",
                L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\GPUCache",
                L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data\\*\\Cache",
                L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data\\*\\Code Cache",
                L"%LOCALAPPDATA%\\Yandex\\YandexBrowser\\User Data\\*\\GPUCache",
                L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Cache",
                L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Code Cache",
                L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Cache",
                L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Code Cache",
                L"%LOCALAPPDATA%\\Opera Software\\Opera Stable\\Cache",
                L"%LOCALAPPDATA%\\Opera Software\\Opera GX Stable\\Cache",
                L"%LOCALAPPDATA%\\Mozilla\\Firefox\\Profiles\\*\\cache2",
            },
        },
        {
            .nameId = IDS_QUICKCLEAN_SHADER,
            .roots = {
                L"%LOCALAPPDATA%\\D3DSCache",
                L"%LOCALAPPDATA%\\NVIDIA\\DXCache",
                L"%LOCALAPPDATA%\\NVIDIA\\GLCache",
                L"%USERPROFILE%\\AppData\\LocalLow\\NVIDIA\\PerDriverVersion\\DXCache",
                L"%LOCALAPPDATA%\\AMD\\DxCache",
                L"%LOCALAPPDATA%\\AMD\\DxcCache",
                L"%LOCALAPPDATA%\\AMD\\GLCache",
                L"%LOCALAPPDATA%\\AMD\\VkCache",
                L"%USERPROFILE%\\AppData\\LocalLow\\Intel\\ShaderCache",
            },
            .checkedByDefault = false,
        },
        {
            .nameId = IDS_QUICKCLEAN_DEV,
            .roots = {
                L"%LOCALAPPDATA%\\npm-cache",
                L"%LOCALAPPDATA%\\pip\\Cache",
                L"%LOCALAPPDATA%\\Yarn\\Cache",
                L"%LOCALAPPDATA%\\NuGet\\v3-cache",
            },
        },
        {
            .nameId = IDS_QUICKCLEAN_UPDATE_CACHE,
            .roots = { L"%SystemRoot%\\SoftwareDistribution\\Download" },
            .needsAdmin = true,
            .checkedByDefault = false,
        },
        {
            // Mods, shader caches and unfinished downloads of games no longer installed in any library
            .nameId = IDS_QUICKCLEAN_STEAM_LEFTOVERS,
            .checkedByDefault = false,
            .dynamicRoots = SteamLeftoverRoots,
            .removeRootFolders = true,
        },
    };
    assert(categories.size() == static_cast<size_t>(categoryCount));
    return categories;
}

std::vector<std::wstring> CQuickCleanDlg::ResolveRoots(const Category& category)
{
    std::vector<std::wstring> result;
    if (category.dynamicRoots != nullptr)
    {
        for (auto& path : category.dynamicRoots())
        {
            if (path.size() > 3 && IsPlainDirectory(path) && std::ranges::find(result, path) == result.end())
                result.push_back(std::move(path));
        }
        return result;
    }

    for (const auto& pattern : category.roots)
    {
        std::wstring expanded(MAX_PATH * 2, L'\0');
        const DWORD length = ExpandEnvironmentStringsW(std::wstring(pattern).c_str(),
            expanded.data(), static_cast<DWORD>(expanded.size()));
        if (length == 0 || length > expanded.size()) continue;
        expanded.resize(length - 1);
        if (expanded.find(L'%') != std::wstring::npos) continue; // unset variable

        // Expand "*" components one folder level at a time
        std::vector<std::wstring> current;
        for (const auto part : std::views::split(expanded, L'\\'))
        {
            const std::wstring component(std::from_range, part);
            if (component.empty()) continue;
            if (current.empty())
            {
                current.push_back(component); // drive, e.g. "C:"
                continue;
            }

            std::vector<std::wstring> next;
            for (const auto& base : current)
            {
                if (component == L"*") next.append_range(ListSubfolders(base));
                else next.push_back(base + L"\\" + component);
            }
            current = std::move(next);
        }

        for (auto& path : current)
        {
            // Require a real path below a drive root: never clean "C:\" itself
            if (path.size() > 3 && IsPlainDirectory(path) && std::ranges::find(result, path) == result.end())
            {
                result.push_back(std::move(path));
            }
        }
    }
    return result;
}

CQuickCleanDlg::Stats CQuickCleanDlg::Process(const Category& category, const bool remove)
{
    FILETIME cutoff{};
    if (category.olderThanDay)
    {
        GetSystemTimeAsFileTime(&cutoff);
        ULONGLONG value = (static_cast<ULONGLONG>(cutoff.dwHighDateTime) << 32) | cutoff.dwLowDateTime;
        value -= 24ULL * 60 * 60 * 10'000'000; // one day in 100 ns units
        cutoff.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFULL);
        cutoff.dwHighDateTime = static_cast<DWORD>(value >> 32);
    }

    Stats stats;
    for (const auto& root : ResolveRoots(category))
    {
        Walk(root, remove, category.olderThanDay ? &cutoff : nullptr, stats);
        if (remove && category.removeRootFolders) RemoveDirectoryW(LongPath(root).c_str()); // only succeeds when emptied
    }
    return stats;
}

CQuickCleanDlg::CQuickCleanDlg(CWnd* pParent)
    : MessageTarget(IDD_QUICKCLEAN, nullptr, pParent)
{
}

bool CQuickCleanDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    Localization::UpdateDialogs(*this);
    SetText(IDC_QUICKCLEAN_INFO, Localization::Lookup(IDS_QUICKCLEAN_INFO));

    const bool elevated = IsElevationActive();
    const auto& categories = GetCategories();
    for (int i = 0; i < categoryCount; ++i)
    {
        const auto& category = categories[static_cast<size_t>(i)];
        std::wstring label = Localization::Lookup(category.nameId);
        if (category.needsAdmin && !elevated) label += L" (" + Localization::Lookup(IDS_QUICKCLEAN_ADMIN) + L")";
        SetText(IDC_QUICKCLEAN_ITEM0 + i, label);
        SetChecked(IDC_QUICKCLEAN_ITEM0 + i, category.checkedByDefault && (elevated || !category.needsAdmin));

        m_layout.AddControl(IDC_QUICKCLEAN_ITEM0 + i, 0, 0, 1, 0);
        m_layout.AddControl(SizeId(i), 1, 0, 0, 0);
    }
    m_layout.AddControl(IDC_QUICKCLEAN_INFO, 0, 0, 1, 0);
    m_layout.AddControl(IDC_QUICKCLEAN_TOTAL, 0, 1, 1, 0);
    m_layout.AddControl(IDOK, 1, 1, 0, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);

    DarkMode::AdjustControls(Handle());

    Measure();
    return true;
}

void CQuickCleanDlg::Measure()
{
    CWaitCursor wait;
    const auto& categories = GetCategories();
    m_stats.assign(categories.size(), {});
    for (int i = 0; i < categoryCount; ++i)
    {
        const size_t index = static_cast<size_t>(i);
        m_stats[index] = Process(categories[index], false);
        const bool hasData = m_stats[index].bytes > 0;
        SetText(SizeId(i), hasData ? FormatBytes(m_stats[index].bytes) : std::wstring(L"\u2014"));
        GetDlgItem(IDC_QUICKCLEAN_ITEM0 + i).EnableWindow(hasData);
    }
    UpdateTotal();
}

void CQuickCleanDlg::UpdateTotal()
{
    ULONGLONG total = 0;
    for (int i = 0; i < categoryCount && static_cast<size_t>(i) < m_stats.size(); ++i)
    {
        if (IsChecked(IDC_QUICKCLEAN_ITEM0 + i) && GetDlgItem(IDC_QUICKCLEAN_ITEM0 + i).IsWindowEnabled())
        {
            total += m_stats[static_cast<size_t>(i)].bytes;
        }
    }
    SetText(IDC_QUICKCLEAN_TOTAL, Localization::Format(IDS_QUICKCLEAN_TOTALs, FormatBytes(total)));
    GetDlgItem(IDOK).EnableWindow(total > 0);
}

void CQuickCleanDlg::OnItemToggled()
{
    UpdateTotal();
}

void CQuickCleanDlg::OnClean()
{
    ULONGLONG total = 0;
    std::vector<size_t> selected;
    for (int i = 0; i < categoryCount && static_cast<size_t>(i) < m_stats.size(); ++i)
    {
        if (!IsChecked(IDC_QUICKCLEAN_ITEM0 + i) || !GetDlgItem(IDC_QUICKCLEAN_ITEM0 + i).IsWindowEnabled()) continue;
        selected.push_back(static_cast<size_t>(i));
        total += m_stats[static_cast<size_t>(i)].bytes;
    }
    if (selected.empty()) return;

    const std::wstring title = Localization::Lookup(IDS_QUICKCLEAN_TITLE);
    if (ShowMessageBox(Handle(), Localization::Format(IDS_QUICKCLEAN_CONFIRMs, FormatBytes(total)),
        title, MB_YESNO | MB_ICONWARNING) != IDYES) return;

    Stats result;
    {
        CWaitCursor wait;
        const auto& categories = GetCategories();
        for (const size_t index : selected)
        {
            const Stats stats = Process(categories[index], true);
            result.bytes += stats.bytes;
            result.files += stats.files;
            result.skipped += stats.skipped;
        }
    }

    if (result.bytes > 0 || result.files > 0) UiSounds::Play(UiSounds::Sound::Purge);
    ShowMessageBox(Handle(), Localization::Format(IDS_QUICKCLEAN_DONEss,
        FormatBytes(result.bytes), std::to_wstring(result.skipped)), title, MB_OK | MB_ICONINFORMATION);

    Measure();
}
