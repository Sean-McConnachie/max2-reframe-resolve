--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so this script gives each .360 file a second name ending in
.mp4 (an NTFS hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it, and imports
those into the current Media Pool bin. Clips are renamed back to the original file name.

Written in Lua because Resolve always includes LuaJIT. Test outside Resolve (linking only):
  set MAX2_TEST_DIR=D:\some\folder
  "C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe" -l lua "Import GoPro 360.lua"
]]

local ffi = require("ffi")
ffi.cdef [[
int MultiByteToWideChar(unsigned int cp, unsigned long flags, const char* s, int n, wchar_t* w, int wn);
int CreateHardLinkW(const wchar_t* link, const wchar_t* target, void* sa);
int CreateDirectoryW(const wchar_t* path, void* sa);
int SetFileAttributesW(const wchar_t* path, unsigned long attrs);
int DeleteFileW(const wchar_t* path);
unsigned long GetLastError(void);
]]
local k32 = ffi.load("kernel32")
local LINK_DIR = "_Max2Reframe"

local function wide(s)
    local n = k32.MultiByteToWideChar(65001, 0, s, -1, nil, 0)
    local buf = ffi.new("wchar_t[?]", n)
    k32.MultiByteToWideChar(65001, 0, s, -1, buf, n)
    return buf
end

local function join(a, b)
    if a:sub(-1) == "\\" or a:sub(-1) == "/" then return a .. b end
    return a .. "\\" .. b
end

-- name -> {IsDir, Size} for the entries of a folder
local function entries(dir)
    local out = {}
    local list = bmd.readdir(join(dir, "*")) or {}
    for _, e in ipairs(list) do
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
        return nil, "error " .. tostring(k32.GetLastError())
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
    print(#files .. " .360 file(s), " .. #links .. " link(s)")
    for _, l in ipairs(links) do print("  " .. l.link) end
    for _, f in ipairs(failed) do print("  failed: " .. f) end
    return
end

local res = resolve or (bmd and bmd.scriptapp and bmd.scriptapp("Resolve"))
if not res then print("Import GoPro 360: could not connect to Resolve") return end
local project = res:GetProjectManager():GetCurrentProject()
if not project then print("Import GoPro 360: open a project first") return end
local fu = fusion or fu or res:Fusion()
local folder = fu and fu:RequestDir(os.getenv("USERPROFILE") or "C:\\")
if not folder or folder == "" then return end

local files, links, failed = linkAll(folder)
if #files == 0 then print("Import GoPro 360: no .360 files in " .. folder) return end

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
print("Import GoPro 360: imported " .. count .. " of " .. #files .. " .360 file(s) from " .. folder)
for _, f in ipairs(failed) do print("  could not link: " .. f) end
