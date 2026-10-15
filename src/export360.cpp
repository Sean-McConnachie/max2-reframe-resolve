#include "export360.h"

#include <windows.h>

#include <cstdint>
#include <vector>

#include "mp4.h"

namespace {

struct VideoSummary
{
    int videoTracks = 0;
    size_t frames = 0;        // samples of the first video track
    bool hasTimecode = false;
    uint32_t tcStart = 0;     // first tmcd sample: start frame number
};

bool summarize(const std::wstring& path, VideoSummary& s, std::string* err)
{
    Mp4File f;
    if (!f.open(path, err)) return false;
    const Mp4Track* tmcd = nullptr;
    for (const auto& t : f.tracks())
    {
        if (t.handler == "vide" && (t.codec == "hvc1" || t.codec == "hev1" || t.codec == "avc1"))
        {
            if (s.videoTracks++ == 0) s.frames = t.sizes.size();
        }
        else if (t.codec == "tmcd" && !tmcd) tmcd = &t;
    }
    std::vector<uint8_t> b;
    if (tmcd && !tmcd->sizes.empty() && f.readSample(*tmcd, 0, b) && b.size() >= 4)
    {
        s.hasTimecode = true;
        s.tcStart = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
    return true;
}

bool isFile(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::vector<std::wstring> subfolders(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd) && out.size() < 1000);
    FindClose(h);
    return out;
}

std::wstring folderOf(const std::wstring& p)
{
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

} // namespace

bool findSource360ForExport(const std::wstring& path, std::wstring& source360, std::string* why)
{
    auto fail = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return fail("the file name has no extension");
    std::wstring base = name.substr(0, dot);
    if (base.size() > 4 && _wcsicmp(base.c_str() + base.size() - 4, L".360") == 0)
        return fail("the file is a link to a .360 file");  // NAME.360.mp4 made by Prepare GoPro 360

    VideoSummary exp;
    std::string err;
    if (!summarize(path, exp, &err)) return fail(err);
    if (exp.videoTracks != 1) return fail("the file is not a single-stream video");

    // Folders to look in, nearest first.
    std::wstring dir = folderOf(path);
    std::wstring parent = folderOf(dir);
    std::vector<std::wstring> dirs{dir};
    if (!parent.empty())
    {
        for (const auto& s : subfolders(parent))
            if (_wcsicmp(s.c_str(), dir.c_str()) != 0) dirs.push_back(s);
        size_t siblings = dirs.size();
        dirs.push_back(parent);
        for (size_t i = 1; i < siblings; ++i)
            for (const auto& s : subfolders(dirs[i])) dirs.push_back(s);
    }

    std::string lastMismatch;
    for (const auto& d : dirs)
    {
        std::wstring cand = d + L"\\" + base + L".360";
        if (!isFile(cand)) continue;
        VideoSummary s;
        if (!summarize(cand, s, &err)) { lastMismatch = err; continue; }
        if (s.videoTracks < 2) { lastMismatch = "a .360 file with the same name has fewer than two video streams"; continue; }
        if (s.frames != exp.frames)
        {
            lastMismatch = "a .360 file with the same name has " + std::to_string(s.frames) + " frames, the export has " +
                           std::to_string(exp.frames);
            continue;
        }
        if (s.hasTimecode && exp.hasTimecode && s.tcStart != exp.tcStart)
        {
            lastMismatch = "a .360 file with the same name has a different start timecode";
            continue;
        }
        source360 = cand;
        return true;
    }
    return fail(lastMismatch.empty() ? "no .360 file with the same name is in the export's folder or the folders next to it"
                                     : lastMismatch);
}
