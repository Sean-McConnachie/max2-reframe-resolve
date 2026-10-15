// Max2Convert.exe: "Convert GoPro Reframe to Max2 Reframe" in the Explorer menu of .drt files.
//   Max2Convert.exe [timeline.drt] [--out file.drt] [--quiet] [--report file.txt] [--exports list.lua]
// Reads a timeline that DaVinci Resolve exported (File > Export > Timeline). Each clip with a GoPro Reframe Fusion
// composition gets the Max2 Reframe effect with the same view instead, and loses the composition. The result goes
// next to the input as "<name> (Max2 Reframe).drt", for File > Import > Timeline. The input file does not change.
// The converted clips still use their GoPro Player exports (the plugin reads the .360 file next to each one). The
// tool adds each export and its .360 file to %LOCALAPPDATA%\Max2Reframe\exports.lua (or --exports), which the
// "Use 360 Files for GoPro Exports" Resolve script reads to replace the exports in the Media Pool.
// Without a file argument the tool shows a file dialog. --quiet shows no message box; --report writes the result
// as UTF-8 text (both for tests).
// Exit code: 0 converted, 1 error, 2 nothing to convert, 3 cancelled.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "drt.h"

namespace {

std::wstring defaultExportList()
{
    PWSTR base = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) p = std::wstring(base) + L"\\Max2Reframe";
    CoTaskMemFree(base);
    return p.empty() ? p : p + L"\\exports.lua";
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(c < 0x80 ? tolower(c) : c); });
    return s;
}

// Adds export -> .360 pairs to the Lua list, keeping the ones that are already in it.
bool updateExportList(const std::wstring& path, const std::vector<std::pair<std::string, std::string>>& add)
{
    std::map<std::string, std::pair<std::string, std::string>> all;
    if (FILE* f = _wfopen(path.c_str(), L"rb"))
    {
        std::string text;
        char buf[4096];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        fclose(f);
        auto value = [&](size_t& at, const char* key, std::string& out) {
            size_t a = text.find(std::string(key) + " = [==[", at);
            if (a == std::string::npos) return false;
            a += strlen(key) + 7;
            size_t b = text.find("]==]", a);
            if (b == std::string::npos) return false;
            out = text.substr(a, b - a);
            at = b + 4;
            return true;
        };
        std::string e, s;
        for (size_t at = 0; value(at, "Export", e) && value(at, "Source", s);) all[lower(e)] = {e, s};
    }
    for (const auto& p : add)
        if (p.first.find("]==]") == std::string::npos && p.second.find("]==]") == std::string::npos) all[lower(p.first)] = p;
    size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fprintf(f, "-- Made by \"Convert GoPro Reframe to Max2 Reframe\" (Max2 Reframe) for the \"Use 360 Files for GoPro\n"
               "-- Exports\" Resolve script. Export: a GoPro Player export in a converted timeline. Source: its .360 file.\n"
               "return {\n");
    for (const auto& kv : all)
        fprintf(f, "  { Export = [==[%s]==], Source = [==[%s]==] },\n", kv.second.first.c_str(), kv.second.second.c_str());
    fprintf(f, "}\n");
    bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

const wchar_t* kTitle = L"Convert GoPro Reframe to Max2 Reframe";

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring pickFile()
{
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    COMDLG_FILTERSPEC filter[] = {{L"DaVinci Resolve timeline (*.drt)", L"*.drt"}};
    dlg->SetFileTypes(1, filter);
    dlg->SetTitle(kTitle);
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item)))
    {
        PWSTR p = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)))
        {
            out = p;
            CoTaskMemFree(p);
        }
        item->Release();
    }
    dlg->Release();
    return out;
}

std::wstring defaultOutput(const std::wstring& in)
{
    size_t dot = in.find_last_of(L'.');
    size_t slash = in.find_last_of(L"\\/");
    std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? in.substr(0, dot) : in;
    return stem + L" (Max2 Reframe).drt";
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring in, out, report, exportList = defaultExportList();
    bool quiet = false;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--quiet") quiet = true;
        else if (a == L"--out" && i + 1 < argc) out = argv[++i];
        else if (a == L"--report" && i + 1 < argc) report = argv[++i];
        else if (a == L"--exports" && i + 1 < argc) exportList = argv[++i];
        else in = a;
    }
    LocalFree(argv);
    if (in.empty() && !quiet) in = pickFile();
    if (in.empty())
    {
        CoUninitialize();
        return 3;
    }
    if (out.empty()) out = defaultOutput(in);

    ConvertResult res;
    std::string err;
    bool ok = convertTimelineFile(in, out, res, &err);
    int code = !ok ? 1 : res.converted ? 0 : 2;
    bool listed = ok && res.converted && !exportList.empty() && updateExportList(exportList, res.sources);

    std::wstring msg;
    UINT icon = MB_ICONINFORMATION;
    if (!ok)
    {
        msg = L"The timeline file was not converted: " + widen(err) + L".";
        icon = MB_ICONERROR;
    }
    else if (res.goproClips == 0)
        msg = L"The timeline has no clips with a GoPro Reframe Fusion composition. No file was made.";
    else
    {
        if (res.converted)
        {
            msg = std::to_wstring(res.converted) + L" of " + std::to_wstring(res.goproClips) +
                  L" clips with GoPro Reframe now use Max2 Reframe.\n\nNew file:\n" + out +
                  L"\n\nIn DaVinci Resolve, select File > Import > Timeline, then select this file. The new timeline is \"" +
                  widen(res.timeline) + L"\".";
            if (!listed) msg += L"\n\nThe list for the \"Use 360 Files for GoPro Exports\" script could not be written.";
        }
        else
            msg = L"No clip was converted, so no file was made.";
        if (!res.notes.empty())
        {
            icon = MB_ICONWARNING;
            msg += L"\n\nThese clips did not change:";
            for (size_t i = 0; i < res.notes.size() && i < 12; ++i) msg += L"\n- " + widen(res.notes[i]);
            if (res.notes.size() > 12) msg += L"\n- and " + std::to_wstring(res.notes.size() - 12) + L" more";
        }
    }
    if (!report.empty())
    {
        FILE* f = _wfopen(report.c_str(), L"wb");
        if (f)
        {
            fprintf(f, "result %d\ngopro %d\nconverted %d\ntimeline %s\nout %s\n", code, res.goproClips, res.converted,
                    res.timeline.c_str(), narrow(out).c_str());
            if (!ok) fprintf(f, "error %s\n", err.c_str());
            for (const auto& n : res.notes) fprintf(f, "note %s\n", n.c_str());
            for (const auto& l : res.log) fprintf(f, "clip %s\n", l.c_str());
            fprintf(f, "listed %d\n", listed ? 1 : 0);
            fclose(f);
        }
    }
    if (!quiet) MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | icon | MB_SETFOREGROUND);
    CoUninitialize();
    return code;
}
