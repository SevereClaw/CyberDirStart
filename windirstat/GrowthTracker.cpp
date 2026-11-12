// CyberDirStat - "What grew": compares folder sizes with an earlier scan
// Based on WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "GrowthTracker.h"
#include "DiskHistory.h"
#include "Item.h"
#include "MessageBoxDlg.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

// Snapshots live in %LOCALAPPDATA%\CyberDirStat\Snapshots, one file per scan of a root:
//   <root hash>_<filetime>.snap, UTF-16 text:
//   line 1:  CDS1 <TAB> <filetime> <TAB> <root path>
//   lines:   <physical size> <TAB> <path relative to the root>   ("" = the root itself)
// Only folders of at least 1 MiB and at most 12 levels deep are stored, which keeps
// a snapshot of a full system drive at a few megabytes.

namespace
{
    using SizeMap = std::unordered_map<std::wstring, ULONGLONG>;

    constexpr ULONGLONG minFolderSize = 1024ULL * 1024;
    constexpr int maxDepth = 12;
    constexpr size_t maxSnapshotsPerRoot = 8;
    constexpr size_t maxGrown = 25, maxShrunk = 10;
    constexpr ULONGLONG oneHour = 60ULL * 60 * 10'000'000; // FILETIME units
    constexpr double explainedShare = 0.8; // a child carrying 80% of the change explains its parent

    struct Snapshot
    {
        std::filesystem::path file;
        ULONGLONG time = 0;
        SizeMap sizes;
    };

    std::mutex s_mutex;
    std::vector<GrowthTracker::Report> s_reports;

    ULONGLONG ToUll(const FILETIME& ft) { return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
    FILETIME ToFileTime(const ULONGLONG v)
    {
        return { .dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFULL), .dwHighDateTime = static_cast<DWORD>(v >> 32) };
    }

    std::filesystem::path SnapshotFolder() { return DiskHistory::StorageFolder(); }
    std::wstring RootKey(const std::wstring& root) { return DiskHistory::RootKey(root); }

    std::vector<Snapshot> LoadAllSnapshots(const std::filesystem::path& folder, const std::wstring& root);

    void Collect(const CItem* item, const std::wstring& relative, const int depth, SizeMap& sizes)
    {
        const ULONGLONG size = item->GetSizePhysical();
        if (size < minFolderSize) return; // children are smaller still
        sizes[relative] = size;
        if (depth >= maxDepth || item->IsLeaf()) return;
        for (const CItem* child : item->GetChildren())
        {
            if (!child->IsTypeOrFlag(IT_DIRECTORY)) continue;
            const std::wstring_view name = child->GetNameView();
            Collect(child, relative.empty() ? std::wstring(name) : relative + L"\\" + std::wstring(name), depth + 1, sizes);
        }
    }

    bool Save(const std::filesystem::path& file, const ULONGLONG time, const std::wstring& root, const SizeMap& sizes)
    {
        std::wstring text = std::format(L"CDS1\t{}\t{}\n", time, root);
        for (const auto& [path, size] : sizes) text += std::format(L"{}\t{}\n", size, path);

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

    std::optional<Snapshot> Load(const std::filesystem::path& file, const std::wstring& root)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) return std::nullopt;
        const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::wstring text(raw.size() / sizeof(wchar_t), L'\0');
        std::memcpy(text.data(), raw.data(), text.size() * sizeof(wchar_t));

        Snapshot snapshot{ .file = file };
        bool header = true;
        for (const auto part : std::views::split(text, L'\n'))
        {
            const std::wstring line(std::from_range, part);
            if (line.empty()) continue;
            const size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos) return std::nullopt;
            if (header)
            {
                const size_t tab2 = line.find(L'\t', tab + 1);
                if (line.substr(0, tab) != L"CDS1" || tab2 == std::wstring::npos) return std::nullopt;
                if (_wcsicmp(line.substr(tab2 + 1).c_str(), root.c_str()) != 0) return std::nullopt; // hash collision
                snapshot.time = _wcstoui64(line.substr(tab + 1, tab2 - tab - 1).c_str(), nullptr, 10);
                header = false;
                continue;
            }
            snapshot.sizes[line.substr(tab + 1)] = _wcstoui64(line.substr(0, tab).c_str(), nullptr, 10);
        }
        if (header || snapshot.time == 0) return std::nullopt;
        return snapshot;
    }

    std::vector<std::filesystem::path> ListSnapshots(const std::filesystem::path& folder, const std::wstring& key)
    {
        std::vector<std::filesystem::path> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
        {
            const std::wstring name = entry.path().filename().wstring();
            if (name.starts_with(key + L"_") && name.ends_with(L".snap")) files.push_back(entry.path());
        }
        return files;
    }

    std::vector<Snapshot> LoadAllSnapshots(const std::filesystem::path& folder, const std::wstring& root)
    {
        std::vector<Snapshot> snapshots;
        for (const auto& file : ListSnapshots(folder, RootKey(root)))
        {
            if (auto snapshot = Load(file, root)) snapshots.push_back(std::move(*snapshot));
        }
        std::ranges::sort(snapshots, {}, &Snapshot::time);
        return snapshots;
    }

    // Folders whose change is at least the threshold and is not mostly explained by one child.
    std::vector<GrowthTracker::Change> Explain(const SizeMap& before, const SizeMap& after, const std::wstring& rootPath, const bool growth)
    {
        std::unordered_map<std::wstring, LONGLONG> delta;
        for (const auto& [path, size] : after) delta[path] += static_cast<LONGLONG>(size);
        for (const auto& [path, size] : before) delta[path] -= static_cast<LONGLONG>(size);

        std::unordered_map<std::wstring, std::vector<std::wstring>> children;
        for (const auto& path : delta | std::views::keys)
        {
            if (path.empty()) continue;
            const size_t slash = path.rfind(L'\\');
            children[slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash)].push_back(path);
        }

        const auto signedChange = [&delta, growth](const std::wstring& path)
        {
            const LONGLONG value = delta.at(path);
            return growth ? value : -value;
        };
        std::vector<std::wstring> candidates;
        for (const auto& [path, change] : delta)
        {
            if (path.empty()) continue; // the root is summarized separately
            const LONGLONG amount = growth ? change : -change;
            if (amount < static_cast<LONGLONG>(GrowthTracker::threshold)) continue;

            bool explained = false;
            if (const auto it = children.find(path); it != children.end())
            {
                for (const auto& child : it->second)
                {
                    if (static_cast<double>(signedChange(child)) >= explainedShare * static_cast<double>(amount)) { explained = true; break; }
                }
            }
            if (explained) continue;
            candidates.push_back(path);
        }

        // A listed folder already covers its subfolders: don't repeat them
        std::vector<GrowthTracker::Change> result;
        const auto lookup = [](const SizeMap& map, const std::wstring& key) { const auto i = map.find(key); return i == map.end() ? 0ULL : i->second; };
        std::wstring prefix = rootPath;
        if (!prefix.ends_with(L'\\')) prefix += L'\\';
        for (const auto& path : candidates)
        {
            const bool covered = std::ranges::any_of(candidates, [&path](const std::wstring& other)
            {
                return path.size() > other.size() && path.starts_with(other) && path[other.size()] == L'\\';
            });
            if (covered) continue;
            result.push_back({ .path = prefix + path, .before = lookup(before, path), .after = lookup(after, path) });
        }

        std::ranges::sort(result, [growth](const auto& a, const auto& b)
        {
            const auto change = [growth](const GrowthTracker::Change& c)
            {
                return growth ? static_cast<LONGLONG>(c.after) - static_cast<LONGLONG>(c.before)
                              : static_cast<LONGLONG>(c.before) - static_cast<LONGLONG>(c.after);
            };
            return change(a) > change(b);
        });
        if (result.size() > (growth ? maxGrown : maxShrunk)) result.resize(growth ? maxGrown : maxShrunk);
        return result;
    }

    GrowthTracker::Report ProcessRoot(const CItem* item, const std::filesystem::path& folder, const ULONGLONG now)
    {
        GrowthTracker::Report report{ .root = item->GetPath() };
        SizeMap current;
        Collect(item, {}, 0, current);
        report.after = item->GetSizePhysical();

        // Fill-up chart: a drive records real volume usage, a folder its scanned size
        ULONGLONG volumeUsed = 0, volumeTotal = 0;
        if (item->IsTypeOrFlag(IT_DRIVE) && DiskHistory::QueryVolume(report.root, volumeUsed, volumeTotal))
            DiskHistory::Append(report.root, volumeUsed, volumeTotal);
        else
            DiskHistory::Append(report.root, report.after, 0);

        const std::wstring key = RootKey(report.root);
        std::vector<Snapshot> snapshots = LoadAllSnapshots(folder, report.root);

        // Baseline: the newest snapshot at least an hour old, so an immediate rescan still compares
        // with the previous session; otherwise the oldest one available.
        const Snapshot* baseline = nullptr;
        for (const auto& snapshot : snapshots) if (snapshot.time + oneHour <= now) baseline = &snapshot;
        if (baseline == nullptr && !snapshots.empty()) baseline = &snapshots.front();

        if (baseline != nullptr)
        {
            report.hasBaseline = true;
            report.baselineTime = ToFileTime(baseline->time);
            const auto rootSize = baseline->sizes.find(L"");
            report.before = rootSize == baseline->sizes.end() ? 0 : rootSize->second;
            report.grown = Explain(baseline->sizes, current, report.root, true);
            report.shrunk = Explain(baseline->sizes, current, report.root, false);
        }

        // Store the current state: replace a snapshot from the last hour, keep at most N per root
        std::error_code ec;
        if (!snapshots.empty() && snapshots.back().time + oneHour > now)
        {
            std::filesystem::remove(snapshots.back().file, ec);
            snapshots.pop_back();
        }
        Save(folder / std::format(L"{}_{}.snap", key, now), now, report.root, current);
        while (snapshots.size() + 1 > maxSnapshotsPerRoot)
        {
            std::filesystem::remove(snapshots.front().file, ec);
            snapshots.erase(snapshots.begin());
        }
        return report;
    }

}

std::wstring GrowthTracker::FormatChange(const Change& change)
{
    const bool grew = change.after >= change.before;
    const std::wstring amount = (grew ? L"+" : L"−") + FormatBytes(grew ? change.after - change.before : change.before - change.after);
    std::wstring detail;
    if (change.before == 0) detail = Localization::Lookup(IDS_GROWTH_NEW);
    else if (change.after == 0) detail = Localization::Lookup(IDS_GROWTH_REMOVED);
    else detail = FormatBytes(change.before) + L" → " + FormatBytes(change.after);
    return std::format(L"{}    {}    ({})", amount, change.path, detail);
}

void GrowthTracker::OnScanFinished(const CItem* root)
{
    if (root == nullptr) return;

    std::vector<const CItem*> roots;
    if (root->IsTypeOrFlag(IT_MYCOMPUTER))
    {
        for (const CItem* child : root->GetChildren()) if (child->IsTypeOrFlag(IT_DRIVE, IT_DIRECTORY)) roots.push_back(child);
    }
    else if (root->IsTypeOrFlag(IT_DRIVE, IT_DIRECTORY))
    {
        roots.push_back(root);
    }

    const std::filesystem::path folder = SnapshotFolder();
    std::error_code ec;
    if (folder.empty()) return;
    std::filesystem::create_directories(folder, ec);
    if (ec) return;

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);

    std::vector<Report> reports;
    for (const CItem* item : roots)
    {
        // Snapshots describe local folders only
        if (!item->SupportsFilesystemApis()) continue;
        reports.push_back(ProcessRoot(item, folder, ToUll(now)));
    }

    std::lock_guard guard(s_mutex);
    s_reports = std::move(reports);
}

GrowthTracker::Report GrowthTracker::LongTermGrowth(const std::wstring& root)
{
    Report report{ .root = root };
    const auto folder = SnapshotFolder();
    if (folder.empty()) return report;

    const auto snapshots = LoadAllSnapshots(folder, root);
    if (snapshots.size() < 2) return report;

    const Snapshot& oldest = snapshots.front();
    const Snapshot& newest = snapshots.back();
    report.hasBaseline = true;
    report.baselineTime = ToFileTime(oldest.time);
    const auto size = [](const Snapshot& s) { const auto i = s.sizes.find(L""); return i == s.sizes.end() ? 0ULL : i->second; };
    report.before = size(oldest);
    report.after = size(newest);
    report.grown = Explain(oldest.sizes, newest.sizes, root, true);
    report.shrunk = Explain(oldest.sizes, newest.sizes, root, false);
    return report;
}

std::vector<GrowthTracker::Report> GrowthTracker::GetReports()
{
    std::lock_guard guard(s_mutex);
    return s_reports;
}

void GrowthTracker::ShowReport()
{
    const auto reports = GetReports();
    const std::wstring title = Localization::Lookup(IDS_GROWTH_TITLE);
    if (reports.empty())
    {
        ShowMessageBox(GetMainWindowHandle(), Localization::Lookup(IDS_GROWTH_NOSCAN), title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring message;
    std::vector<std::wstring> lines;
    for (const auto& report : reports)
    {
        if (!message.empty()) message += L"\n";
        if (!report.hasBaseline)
        {
            message += Localization::Format(IDS_GROWTH_FIRSTs, report.root);
            continue;
        }

        const bool grew = report.after >= report.before;
        const std::wstring total = (grew ? L"+" : L"−") +
            FormatBytes(grew ? report.after - report.before : report.before - report.after);
        message += Localization::Format(IDS_GROWTH_HEADERsssss, report.root, FormatBytes(report.before),
            FormatBytes(report.after), total, FormatFileTime(report.baselineTime));

        if (report.grown.empty() && report.shrunk.empty())
        {
            lines.push_back(report.root + L":  " + Localization::Format(IDS_GROWTH_NOCHANGEs, FormatBytes(threshold)));
            continue;
        }
        if (!report.grown.empty())
        {
            lines.push_back(L"▲ " + Localization::Lookup(IDS_GROWTH_GROWN));
            for (const auto& change : report.grown) lines.push_back(L"    " + FormatChange(change));
        }
        if (!report.shrunk.empty())
        {
            lines.push_back(L"▼ " + Localization::Lookup(IDS_GROWTH_SHRUNK));
            for (const auto& change : report.shrunk) lines.push_back(L"    " + FormatChange(change));
        }
    }

    if (lines.empty())
    {
        ShowMessageBox(GetMainWindowHandle(), message, title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    CMessageBoxDlg::Show(message, lines, {}, false, MB_OK | MB_ICONINFORMATION, {}, CSize(720, 520), title);
}
