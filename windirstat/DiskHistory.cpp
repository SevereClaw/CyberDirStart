// CyberDirStat - history of used space per drive/folder (for the fill-up chart)
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "DiskHistory.h"

#include <fstream>
#include <mutex>

// File per root: <root key>.history, UTF-16 text:
//   line 1: CDH1 <TAB> <root path>
//   lines:  <filetime> <TAB> <used bytes> <TAB> <total bytes>
// Written by complete scans and by the tray monitor (hourly for fixed drives).

namespace
{
    constexpr ULONGLONG mergeWindow = 30ULL * 60 * 10'000'000; // 30 minutes in FILETIME units
    constexpr size_t maxPoints = 3000;                          // ~4 months of hourly points

    std::mutex s_mutex;

    std::filesystem::path HistoryFile(const std::wstring& root)
    {
        const auto folder = DiskHistory::StorageFolder();
        return folder.empty() ? folder : folder / (DiskHistory::RootKey(root) + L".history");
    }

    std::wstring ReadUtf16(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) return {};
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::wstring text(raw.size() / sizeof(wchar_t), L'\0');
        std::memcpy(text.data(), raw.data(), text.size() * sizeof(wchar_t));
        return text;
    }

    bool WriteUtf16(const std::filesystem::path& file, const std::wstring& text)
    {
        const std::filesystem::path temp = file.wstring() + L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
            if (!out) return false;
        }
        std::error_code ec;
        std::filesystem::rename(temp, file, ec);
        return !ec;
    }

    ULONGLONG Now()
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
}

std::filesystem::path DiskHistory::StorageFolder()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()) / L"CyberDirStat" / L"Snapshots";
}

std::wstring DiskHistory::RootKey(const std::wstring& root)
{
    std::wstring lower = root;
    std::ranges::transform(lower, lower.begin(), [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return std::format(L"{:016x}", std::hash<std::wstring>{}(lower));
}

std::wstring DiskHistory::Serialize(const std::wstring& root, const std::vector<Point>& points)
{
    std::wstring text = L"CDH1\t" + root + L"\n";
    for (const auto& p : points) text += std::format(L"{}\t{}\t{}\n", p.time, p.used, p.total);
    return text;
}

bool DiskHistory::Parse(const std::wstring& text, std::wstring& root, std::vector<Point>& points)
{
    points.clear();
    bool header = true;
    for (const auto part : std::views::split(text, L'\n'))
    {
        const std::wstring line(std::from_range, part);
        if (line.empty()) continue;
        if (header)
        {
            if (!line.starts_with(L"CDH1\t")) return false;
            root = line.substr(5);
            header = false;
            continue;
        }
        const size_t a = line.find(L'\t'), b = a == std::wstring::npos ? a : line.find(L'\t', a + 1);
        if (b == std::wstring::npos) continue; // skip a damaged line, keep the rest
        points.push_back({
            .time = _wcstoui64(line.substr(0, a).c_str(), nullptr, 10),
            .used = _wcstoui64(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10),
            .total = _wcstoui64(line.substr(b + 1).c_str(), nullptr, 10) });
    }
    std::ranges::sort(points, {}, &Point::time);
    return !header;
}

void DiskHistory::AddPoint(std::vector<Point>& points, const Point& point)
{
    if (!points.empty() && point.time >= points.back().time && point.time - points.back().time < mergeWindow)
        points.back() = point;
    else
        points.push_back(point);
    if (points.size() > maxPoints) points.erase(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(points.size() - maxPoints));
}

void DiskHistory::Append(const std::wstring& root, const ULONGLONG used, const ULONGLONG total)
{
    const auto file = HistoryFile(root);
    if (file.empty() || root.empty()) return;

    std::lock_guard guard(s_mutex);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::wstring storedRoot;
    std::vector<Point> points;
    if (!Parse(ReadUtf16(file), storedRoot, points) || _wcsicmp(storedRoot.c_str(), root.c_str()) != 0) points.clear();
    AddPoint(points, { .time = Now(), .used = used, .total = total });
    WriteUtf16(file, Serialize(root, points));
}

std::vector<DiskHistory::Point> DiskHistory::Load(const std::wstring& root)
{
    const auto file = HistoryFile(root);
    if (file.empty()) return {};
    std::lock_guard guard(s_mutex);
    std::wstring storedRoot;
    std::vector<Point> points;
    if (!Parse(ReadUtf16(file), storedRoot, points) || _wcsicmp(storedRoot.c_str(), root.c_str()) != 0) return {};
    return points;
}

std::vector<std::wstring> DiskHistory::Roots()
{
    std::vector<std::wstring> roots;
    const auto folder = StorageFolder();
    if (folder.empty()) return roots;

    std::lock_guard guard(s_mutex);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        if (entry.path().extension() != L".history") continue;
        std::wstring root;
        std::vector<Point> points;
        if (Parse(ReadUtf16(entry.path()), root, points) && !points.empty()) roots.push_back(root);
    }
    std::ranges::sort(roots, [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return roots;
}

bool DiskHistory::QueryVolume(const std::wstring& root, ULONGLONG& used, ULONGLONG& total)
{
    ULARGE_INTEGER available{}, capacity{}, free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, &capacity, &free) || capacity.QuadPart == 0) return false;
    total = capacity.QuadPart;
    used = capacity.QuadPart - std::min(free.QuadPart, capacity.QuadPart);
    return true;
}
