--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so this script gives each .360 file a second name ending in
.mp4 (an NTFS hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it, and imports
those into the current Media Pool bin. Clips are renamed back to the original file name.

Resolve runs menu scripts in a restricted Lua: there is no require (so no ffi) and no io. This script uses only
bmd.*, fu:RequestDir and os.execute (cmd's mklink).

Progress and errors go to the Console (Workspace > Console) and to
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\Max2Reframe-import.log.

Test outside Resolve (linking only):
  set MAX2_TEST_DIR=D:\some\folder
  "C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe" -l lua "Import GoPro 360.lua"
]]

local LINK_DIR = "_Max2Reframe"
local appData = os.getenv("APPDATA") or ""
local supportDir = appData .. [[\Blackmagic Design\DaVinci Resolve\Support]]
local logPath = supportDir .. [[\logs\Max2Reframe-import.log]]
local settingsPath = supportDir .. [[\Max2Reframe-import.settings]]

local logLines = {}
local function log(msg)
    msg = tostring(msg)
    print("Import GoPro 360: " .. msg)
    logLines[#logLines + 1] = os.date("%Y-%m-%d %H:%M:%S ") .. msg
    pcall(bmd.writefile, logPath, {Log = logLines})
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

-- Run cmd commands joined with "&", in batches that stay under cmd's 8191 character line limit.
local function runCmds(cmds)
    local batch, len = {}, 0
    local function flush()
        if #batch > 0 then os.execute(table.concat(batch, " & ")) end
        batch, len = {}, 0
    end
    for _, c in ipairs(cmds) do
        if len + #c + 3 > 7000 then flush() end
        batch[#batch + 1] = c
        len = len + #c + 3
    end
    flush()
end

-- Make the .mp4 hard links for a list of .360 files. Returns the links and the failures.
local function linkAll(files)
    local links, failed, cmds, todo = {}, {}, {}, {}
    local hidden = {}
    for _, f in ipairs(files) do
        if sizeOf(f.dir, f.name .. ".mp4") == f.size then
            -- a link made by tools\link-mp4.ps1 next to the file
            f.link = join(f.dir, f.name .. ".mp4")
        else
            local linkDir = join(f.dir, LINK_DIR)
            f.link = join(linkDir, f.name .. ".mp4")
            local existing = bmd.direxists(linkDir) and sizeOf(linkDir, f.name .. ".mp4")
            if existing ~= f.size then
                if not bmd.direxists(linkDir) then
                    bmd.createdir(linkDir)
                    if not hidden[linkDir] then
                        hidden[linkDir] = true
                        cmds[#cmds + 1] = 'attrib +h "' .. linkDir .. '"'
                    end
                end
                if existing then cmds[#cmds + 1] = 'del /f /q "' .. f.link .. '"' end -- the .360 was replaced
                cmds[#cmds + 1] = 'mklink /H "' .. f.link .. '" "' .. f.path .. '" >nul'
                todo[#todo + 1] = f
            end
        end
    end
    if #cmds > 0 then runCmds(cmds) end
    for _, f in ipairs(files) do
        local dir, name = f.link:match("^(.*)\\([^\\]*)$")
        if sizeOf(dir, name) == f.size then links[#links + 1] = f
        else failed[#failed + 1] = f.path end
    end
    return links, failed
end

local function lastFolder()
    local ok, t = pcall(bmd.readfile, settingsPath)
    return ok and type(t) == "table" and t.LastFolder or nil
end

local function main()
    log("started")

    -- Test mode outside Resolve
    local testDir = os.getenv("MAX2_TEST_DIR")
    if testDir and not resolve then
        local files = find360(testDir, {})
        local links, failed = linkAll(files)
        log(#files .. " .360 file(s), " .. #links .. " link(s)")
        for _, f in ipairs(links) do log("  " .. f.link) end
        for _, p in ipairs(failed) do log("  failed: " .. p) end
        return
    end

    local res = resolve or (bmd.scriptapp and bmd.scriptapp("Resolve"))
    if not res then log("could not connect to Resolve") return end
    local project = res:GetProjectManager():GetCurrentProject()
    if not project then log("open a project first") return end
    if type(os.execute) ~= "function" then log("this Resolve version does not allow os.execute, so links cannot be made") return end

    local fusionApp = fu or fusion or res:Fusion()
    if not fusionApp then log("could not get the Fusion object for the folder browser") return end
    log("opening the folder browser")
    local folder = fusionApp:RequestDir(lastFolder() or "", {FReqS_Title = "Select the folder with your .360 files"})
    if not folder or folder == "" then log("no folder selected") return end
    folder = folder:gsub("/", "\\")
    pcall(bmd.writefile, settingsPath, {LastFolder = folder})
    log("scanning " .. folder .. " and its subfolders")

    local files = find360(folder, {})
    if #files == 0 then log("no .360 files in " .. folder) return end
    local links, failed = linkAll(files)
    for _, p in ipairs(failed) do log("could not make a link for " .. p .. " (is the drive NTFS?)") end
    if #links == 0 then return end

    local paths, names = {}, {}
    for i, f in ipairs(links) do
        paths[i] = f.link
        names[f.link:lower()] = f.name
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
