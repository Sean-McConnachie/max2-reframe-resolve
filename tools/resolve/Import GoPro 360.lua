--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so this script gives each .360 file a second name ending in
.mp4 (an NTFS hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it, and imports
those into the current Media Pool bin. Clips are renamed back to the original file name.

Progress and errors go to the Console (Workspace > Console) and to
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\Max2Reframe-import.log.

Written in Lua because Resolve always includes LuaJIT. Test outside Resolve (linking only):
  set MAX2_TEST_DIR=D:\some\folder
  "C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe" -l lua "Import GoPro 360.lua"
]]

local LINK_DIR = "_Max2Reframe"
-- next to Resolve's own logs
local logDir = os.getenv("APPDATA") and (os.getenv("APPDATA") .. "\\Blackmagic Design\\DaVinci Resolve\\Support\\logs")
local logPath = (logDir or os.getenv("TEMP") or "C:\\Windows\\Temp") .. "\\Max2Reframe-import.log"

local function log(msg)
    msg = "Import GoPro 360: " .. tostring(msg)
    print(msg)
    local f = io.open(logPath, "a")
    if f then
        f:write(os.date("%Y-%m-%d %H:%M:%S "), msg, "\n")
        f:close()
    end
end

local function main()
    local ffi = require("ffi")
    -- Resolve may run every script in the same Lua state, so declarations from an earlier run can still exist:
    -- declare each one separately and ignore "attempt to redefine".
    local decls = {
        "int MultiByteToWideChar(unsigned int cp, unsigned long flags, const char* s, int n, wchar_t* w, int wn);",
        "int WideCharToMultiByte(unsigned int cp, unsigned long flags, const wchar_t* w, int wn, char* s, int n, const char* d, int* u);",
        "int CreateHardLinkW(const wchar_t* link, const wchar_t* target, void* sa);",
        "int CreateDirectoryW(const wchar_t* path, void* sa);",
        "int SetFileAttributesW(const wchar_t* path, unsigned long attrs);",
        "int DeleteFileW(const wchar_t* path);",
        "unsigned long GetLastError(void);",
        "typedef struct { void* hwndOwner; void* pidlRoot; wchar_t* pszDisplayName; const wchar_t* lpszTitle;"
            .. " unsigned int ulFlags; void* lpfn; intptr_t lParam; int iImage; } MAX2_BROWSEINFOW;",
        "void* SHBrowseForFolderW(MAX2_BROWSEINFOW* bi);",
        "int SHGetPathFromIDListW(void* pidl, wchar_t* path);",
        "long CoInitializeEx(void* reserved, unsigned long coinit);",
        "void CoTaskMemFree(void* p);",
        "void* GetForegroundWindow(void);",
    }
    for _, d in ipairs(decls) do pcall(ffi.cdef, d) end
    local k32, shell32, ole32, user32 = ffi.load("kernel32"), ffi.load("shell32"), ffi.load("ole32"), ffi.load("user32")

    local function wide(s)
        local n = k32.MultiByteToWideChar(65001, 0, s, -1, nil, 0)
        local buf = ffi.new("wchar_t[?]", n)
        k32.MultiByteToWideChar(65001, 0, s, -1, buf, n)
        return buf
    end

    local function utf8(w)
        local n = k32.WideCharToMultiByte(65001, 0, w, -1, nil, 0, nil, nil)
        local buf = ffi.new("char[?]", n)
        k32.WideCharToMultiByte(65001, 0, w, -1, buf, n, nil, nil)
        return ffi.string(buf)
    end

    -- Windows folder browser (Fusion's RequestDir does not open from the Edit page). Returns nil on cancel.
    local function pickFolder(title)
        ole32.CoInitializeEx(nil, 0x2) -- apartment threaded; harmless if COM is already initialised
        local name = ffi.new("wchar_t[260]")
        local bi = ffi.new("MAX2_BROWSEINFOW")
        bi.hwndOwner = user32.GetForegroundWindow()
        bi.pszDisplayName = name
        local t = wide(title)
        bi.lpszTitle = t
        bi.ulFlags = 0x1 + 0x40 + 0x200 -- file system folders only, new dialog style, no "new folder" button
        local pidl = shell32.SHBrowseForFolderW(bi)
        if pidl == nil then return nil end
        local path = ffi.new("wchar_t[32768]")
        local ok = shell32.SHGetPathFromIDListW(pidl, path) ~= 0
        ole32.CoTaskMemFree(pidl)
        return ok and utf8(path) or nil
    end

    local function join(a, b)
        if a:sub(-1) == "\\" or a:sub(-1) == "/" then return a .. b end
        return a .. "\\" .. b
    end

    local function entries(dir)
        local out = {}
        for _, e in ipairs(bmd.readdir(join(dir, "*")) or {}) do
            if e.Name ~= "." and e.Name ~= ".." then out[#out + 1] = e end
        end
        return out
    end

    local function find360(dir, found)
        for _, e in ipairs(entries(dir)) do
            local path = join(dir, e.Name)
            if e.IsDir then
                if e.Name ~= LINK_DIR then find360(path, found) end
            elseif e.Name:lower():sub(-4) == ".360" then
                found[#found + 1] = {path = path, dir = dir, name = e.Name, size = e.Size}
            end
        end
        return found
    end

    local function sizeOf(dir, name)
        for _, e in ipairs(entries(dir)) do
            if e.Name:lower() == name:lower() then return e.Size end
        end
        return nil
    end

    -- Create (if needed) and return the .mp4 hard link for a .360 file, or nil and an error.
    local function linkFor(f)
        -- links made by tools\link-mp4.ps1 sit next to the file; reuse those
        if sizeOf(f.dir, f.name .. ".mp4") == f.size then return join(f.dir, f.name .. ".mp4") end
        local linkDir = join(f.dir, LINK_DIR)
        if not bmd.fileexists(linkDir) then
            k32.CreateDirectoryW(wide(linkDir), nil)
            k32.SetFileAttributesW(wide(linkDir), 0x2) -- hidden
        end
        local link = join(linkDir, f.name .. ".mp4")
        local existing = sizeOf(linkDir, f.name .. ".mp4")
        if existing == f.size then return link end
        if existing then k32.DeleteFileW(wide(link)) end -- stale: the .360 was replaced since
        if k32.CreateHardLinkW(wide(link), wide(f.path), nil) == 0 then
            return nil, "Windows error " .. tostring(k32.GetLastError())
        end
        return link
    end

    local function linkAll(folder)
        local files = find360(folder, {})
        local links, failed = {}, {}
        for _, f in ipairs(files) do
            local link, err = linkFor(f)
            if link then links[#links + 1] = {link = link, name = f.name}
            else failed[#failed + 1] = f.path .. " (" .. err .. ")" end
        end
        return files, links, failed
    end

    -- Test mode outside Resolve
    local testDir = os.getenv("MAX2_TEST_DIR")
    if testDir and not resolve then
        local files, links, failed = linkAll(testDir)
        log(#files .. " .360 file(s), " .. #links .. " link(s)")
        for _, l in ipairs(links) do log("  " .. l.link) end
        for _, f in ipairs(failed) do log("  failed: " .. f) end
        return
    end

    log("started")
    local res = resolve or (bmd and bmd.scriptapp and bmd.scriptapp("Resolve"))
    if not res then log("could not connect to Resolve") return end
    local project = res:GetProjectManager():GetCurrentProject()
    if not project then log("open a project first") return end

    log("opening the folder browser")
    local folder = pickFolder("Select the folder with your .360 files. Subfolders are included.")
    if not folder or folder == "" then log("no folder selected") return end
    log("scanning " .. folder)

    local files, links, failed = linkAll(folder)
    for _, f in ipairs(failed) do log("could not link: " .. f) end
    if #files == 0 then log("no .360 files in " .. folder) return end

    local paths, names = {}, {}
    for i, l in ipairs(links) do
        paths[i] = l.link
        names[l.link:lower()] = l.name
    end
    local items = project:GetMediaPool():ImportMedia(paths) or {}
    local count = 0
    for _, item in pairs(items) do
        count = count + 1
        local path = item:GetClipProperty("File Path")
        local name = path and names[path:lower()]
        if name then pcall(function() item:SetClipProperty("Clip Name", name) end) end
    end
    log("imported " .. count .. " of " .. #files .. " .360 file(s) from " .. folder)
end

local ok, err = xpcall(main, debug and debug.traceback or function(e) return e end)
if not ok then log("ERROR: " .. tostring(err)) end
