// Max2Prepare.exe: "Prepare GoPro 360 for Resolve" in the Explorer folder menu.
//   Max2Prepare.exe [folder] [--quiet] [--links dir]
// DaVinci Resolve does not import files with the .360 extension, and its menu scripts (Free 21.1) cannot create
// files or start programs. So this tool does that part outside Resolve. For each NAME.360 in the folder and its
// subfolders it makes a second name, NAME.360.mp4, that Resolve imports:
//  - On NTFS: a hard link in a hidden "_Max2Reframe" folder next to the file (the same file, no extra disk space).
//  - On drives without hard links (exFAT, FAT32): a symbolic link in %LOCALAPPDATA%\Max2Reframe\links (or --links),
//    in a folder path that mirrors the original one. Without Developer Mode, Windows lets only an administrator make
//    symbolic links, so the tool then runs itself again with a permission prompt.
// Each "_Max2Reframe" folder also gets files.lua, the list of links, which the "Import GoPro 360" Resolve scripts read
// with dofile(). Without a folder argument the tool shows a folder dialog. --quiet shows no message box (for tests).
// Exit code: 0 done, 1 errors, 2 no .360 files found, 3 cancelled.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

const wchar_t* kTitle = L"Prepare GoPro 360 for Resolve";
const wchar_t* kLinkDir = L"_Max2Reframe";

struct Result
{
    int files = 0;    // .360 files with a link
    int folders = 0;  // folders with .360 files
    int symlinks = 0; // files linked with a symbolic link (drive without hard links)
    bool needAdmin = false; // Windows refused a symbolic link without administrator rights
    std::vector<std::wstring> errors;
};

struct Entry
{
    std::wstring name; // NAME.360
    std::wstring link; // full path of a symbolic link; empty for a hard link in the _Max2Reframe folder
};

bool endsWithI(const std::wstring& s, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

// Extended-length form, so paths longer than MAX_PATH also work.
std::wstring ext(const std::wstring& path)
{
    if (path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring errorText(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"error " + std::to_wstring(code);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(ext(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

// True when both paths name the same file (the link already points to this .360). Opening a symbolic link opens its
// target, so this works for both kinds of link.
bool sameFile(const std::wstring& a, const std::wstring& b)
{
    auto id = [](const std::wstring& p, BY_HANDLE_FILE_INFORMATION& info) {
        HANDLE h = CreateFileW(ext(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool ok = GetFileInformationByHandle(h, &info) != 0;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION ia{}, ib{};
    return id(a, ia) && id(b, ib) && ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
}

bool supportsHardLinks(const std::wstring& dir)
{
    wchar_t vol[MAX_PATH];
    DWORD flags = 0;
    if (!GetVolumePathNameW((dir + L"\\").c_str(), vol, MAX_PATH)) return true;
    if (!GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) return true;
    return (flags & FILE_SUPPORTS_HARD_LINKS) != 0;
}

bool isElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    bool elevated = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    if (token) CloseHandle(token);
    return elevated;
}

// Where the symbolic links for dir go: E:\Trip\Day 1 -> <links>\E\Trip\Day 1, \\nas\share\x -> <links>\UNC\nas\share\x
std::wstring mirrorDir(const std::wstring& links, const std::wstring& dir)
{
    if (dir.rfind(L"\\\\", 0) == 0) return links + L"\\UNC\\" + dir.substr(2);
    if (dir.size() >= 2 && dir[1] == L':') return links + L"\\" + dir.substr(0, 1) + dir.substr(2);
    return links + L"\\" + dir;
}

bool createDirs(const std::wstring& path)
{
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(ext(path.substr(0, i)).c_str(), nullptr);
    return exists(path);
}

bool writeIndex(const std::wstring& linkDir, const std::vector<Entry>& entries, Result& r)
{
    std::string text = "-- Made by \"Prepare GoPro 360 for Resolve\" (Max2 Reframe) for the Import GoPro 360 scripts.\n"
                       "-- Name: a .360 file in the folder above. Link: its .mp4 link; without Link it is\n"
                       "-- Name .. \".mp4\" in this folder.\nreturn {\n";
    for (const auto& e : entries)
    {
        text += "  { Name = [==[" + utf8(e.name) + "]==]";
        if (!e.link.empty()) text += ", Link = [==[" + utf8(e.link) + "]==]";
        text += " },\n";
    }
    text += "}\n";
    std::wstring path = linkDir + L"\\files.lua";
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(ext(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr) &&
              written == text.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    // write then rename, so a script never reads half a file
    if (ok) ok = MoveFileExW(ext(tmp).c_str(), ext(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) r.errors.push_back(path + L": " + errorText(GetLastError()));
    return ok;
}

void prepareFolder(const std::wstring& dir, const std::vector<std::wstring>& files, const std::wstring& links, Result& r)
{
    std::wstring linkDir = dir + L"\\" + kLinkDir;
    if (!CreateDirectoryW(ext(linkDir).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        r.errors.push_back(linkDir + L": " + errorText(GetLastError()));
        return;
    }
    DWORD attrs = GetFileAttributesW(ext(linkDir).c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_HIDDEN))
        SetFileAttributesW(ext(linkDir).c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);

    bool hard = supportsHardLinks(dir);
    std::wstring symDir = hard ? L"" : mirrorDir(links, dir);
    if (!hard && !createDirs(symDir))
    {
        r.errors.push_back(symDir + L": " + errorText(GetLastError()));
        return;
    }

    std::vector<Entry> linked;
    for (const auto& name : files)
    {
        std::wstring src = dir + L"\\" + name;
        std::wstring link = (hard ? linkDir : symDir) + L"\\" + name + L".mp4";
        Entry entry{name, hard ? L"" : link};
        if (exists(link))
        {
            if (sameFile(src, link))
            {
                linked.push_back(entry);
                continue;
            }
            // the .360 was replaced or moved since the link was made
            DeleteFileW(ext(link).c_str());
        }
        bool ok = hard ? CreateHardLinkW(ext(link).c_str(), ext(src).c_str(), nullptr) != 0
                       : CreateSymbolicLinkW(ext(link).c_str(), src.c_str(),
                                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
        if (ok)
        {
            linked.push_back(entry);
            continue;
        }
        DWORD e = GetLastError();
        if (!hard && e == ERROR_PRIVILEGE_NOT_HELD)
        {
            r.needAdmin = true; // the caller runs everything again with administrator rights
            return;
        }
        r.errors.push_back(src + L": " + errorText(e));
    }
    if (writeIndex(linkDir, linked, r) && !linked.empty())
    {
        r.files += int(linked.size());
        if (!hard) r.symlinks += int(linked.size());
        ++r.folders;
    }
}

void walk(const std::wstring& root, const std::wstring& links, Result& r)
{
    std::vector<std::wstring> stack{root};
    while (!stack.empty() && !r.needAdmin)
    {
        std::wstring dir = stack.back();
        stack.pop_back();
        std::vector<std::wstring> files, subdirs;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(ext(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE)
        {
            if (dir == root) r.errors.push_back(dir + L": " + errorText(GetLastError()));
            continue;
        }
        do
        {
            std::wstring name = fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                // skip ".", "..", our own link folders, junctions (they can loop) and system folders
                if (name == L"." || name == L".." || _wcsicmp(name.c_str(), kLinkDir) == 0) continue;
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SYSTEM)) continue;
                subdirs.push_back(dir + L"\\" + name);
            }
            else if (endsWithI(name, L".360"))
            {
                files.push_back(name);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
            return _wcsicmp(a.c_str(), b.c_str()) < 0;
        });
        if (!files.empty())
            prepareFolder(dir, files, links, r);
        else if (exists(dir + L"\\" + kLinkDir + L"\\files.lua"))
            writeIndex(dir + L"\\" + kLinkDir, {}, r); // the .360 files were moved away
        for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) stack.push_back(*it);
    }
}

// Runs this tool again with administrator rights (one permission prompt) and returns its exit code, or -1.
int runElevated(const std::wstring& folder, const std::wstring& links, bool quiet)
{
    wchar_t exe[32768];
    if (!GetModuleFileNameW(nullptr, exe, 32768)) return -1;
    std::wstring params = L"\"" + folder + L"\" --links \"" + links + L"\" --elevated" + (quiet ? L" --quiet" : L"");
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return int(code);
}

std::wstring defaultLinksDir()
{
    std::wstring dir;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
    {
        dir = std::wstring(base) + L"\\Max2Reframe\\links";
        CoTaskMemFree(base);
    }
    return dir;
}

std::wstring pickFolder()
{
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Prepare GoPro 360 for Resolve: select the folder with your .360 files");
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(nullptr)) && SUCCEEDED(dlg->GetResult(&item)))
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            result = path;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
    {
        // "D:\" ends in \" which Windows reads as an escaped quote, so the folder arrives as D:" plus the rest of
        // the command line. Cut it at the quote and read the rest as more arguments.
        std::wstring a = argv[i];
        size_t q = a.find(L'"');
        if (q == std::wstring::npos)
        {
            args.push_back(a);
            continue;
        }
        args.push_back(a.substr(0, q));
        std::wstring rest = a.substr(q + 1);
        for (size_t p = 0; p < rest.size();)
        {
            size_t e = rest.find(L' ', p);
            if (e == std::wstring::npos) e = rest.size();
            if (e > p) args.push_back(rest.substr(p, e - p));
            p = e + 1;
        }
    }
    LocalFree(argv);
    std::wstring folder, links;
    bool quiet = false, elevated = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& a = args[i];
        if (_wcsicmp(a.c_str(), L"--quiet") == 0) quiet = true;
        else if (_wcsicmp(a.c_str(), L"--elevated") == 0) elevated = true;
        else if (_wcsicmp(a.c_str(), L"--links") == 0 && i + 1 < args.size()) links = args[++i];
        else if (!a.empty()) folder = a;
    }
    if (links.empty()) links = defaultLinksDir();
    if (folder.empty()) folder = pickFolder();
    if (folder.empty()) return 3;

    static wchar_t full[32768];
    DWORD n = GetFullPathNameW(folder.c_str(), 32768, full, nullptr);
    if (n > 0 && n < 32768) folder = full;
    // "D:\" becomes "D:", so "D:" + "\name" is still a valid path
    while (folder.size() > 1 && folder.back() == L'\\') folder.pop_back();

    Result r;
    walk(folder, links, r);
    if (r.needAdmin && !elevated && !isElevated())
    {
        // The elevated copy shows its own message. Pass the links folder, because an administrator account that is
        // not this user has a different %LOCALAPPDATA%.
        int code = runElevated(folder, links, quiet);
        if (code >= 0) return code;
        r.errors.push_back(L"Windows did not allow the symbolic links. Click Yes in the permission prompt, or turn on "
                           L"Developer Mode in Windows Settings (For developers), then prepare the folder again.");
    }
    else if (r.needAdmin)
    {
        r.errors.push_back(L"Windows did not allow a symbolic link in " + links + L".");
    }

    std::wstring msg;
    UINT icon = MB_ICONINFORMATION;
    if (r.files == 0 && r.errors.empty())
    {
        msg = L"No .360 files found in\n" + folder + L"\nor its subfolders.";
        icon = MB_ICONWARNING;
    }
    else
    {
        msg = L"Prepared " + std::to_wstring(r.files) + L" .360 file(s) in " + std::to_wstring(r.folders) +
              L" folder(s) of\n" + folder + L"\n\nIn DaVinci Resolve, select Workspace > Scripts > Import GoPro 360 "
              L"(this folder only) or Import GoPro 360 Folder Tree (with subfolders), then select this folder.";
        if (r.symlinks > 0)
            msg += L"\n\nThis drive cannot hold hard links (it is not NTFS), so the links are symbolic links in\n" +
                   links + L"\nIf the drive gets a different letter, prepare the folder again.";
    }
    if (!r.errors.empty())
    {
        icon = MB_ICONERROR;
        msg += L"\n\n" + std::to_wstring(r.errors.size()) + L" problem(s):";
        for (size_t i = 0; i < r.errors.size() && i < 8; ++i) msg += L"\n- " + r.errors[i];
        if (r.errors.size() > 8) msg += L"\n...";
    }
    if (!quiet) MessageBoxW(nullptr, msg.c_str(), kTitle, MB_OK | icon | MB_SETFOREGROUND);
    CoUninitialize();
    if (!r.errors.empty()) return 1;
    return r.files == 0 ? 2 : 0;
}
