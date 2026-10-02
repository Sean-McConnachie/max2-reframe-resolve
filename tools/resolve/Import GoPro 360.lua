--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so each .360 file gets a second name ending in .mp4 (an NTFS
hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it. This script imports
those links into the current Media Pool bin and renames the clips back to the original file names.

Resolve runs menu scripts in a restricted Lua (no require, no io, and Fusion's folder dialog does not open from the
Edit page). So the folder dialog and the links are made by "Import GoPro 360.ps1", which the installer puts in
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Max2Reframe. It writes its log (import.log) there.

Failures are raised as Lua errors, so they also appear in Resolve's own log (Support\logs\ResolveDebug.txt).

Test outside Resolve (no dialog, no import):
  set MAX2_TEST_DIR=D:\some\folder
  "C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe" -l lua "Import GoPro 360.lua"
]]

local helperDir = (os.getenv("APPDATA") or "") .. [[\Blackmagic Design\DaVinci Resolve\Support\Max2Reframe]]
local helper = helperDir .. [[\Import GoPro 360.ps1]]
local resultPath = helperDir .. [[\result.txt]]

local function log(msg) print("Import GoPro 360: " .. tostring(msg)) end
local function fail(msg) error("Import GoPro 360: " .. msg, 0) end

local function main()
    log("started")
    if type(os.execute) ~= "function" then fail("os.execute is not available in this version of Resolve") end
    if not bmd.fileexists(helper) then fail("missing " .. helper .. ". Run Install.bat again.") end

    local testDir = os.getenv("MAX2_TEST_DIR")
    local res = resolve or (bmd.scriptapp and bmd.scriptapp("Resolve"))
    local project = res and res:GetProjectManager():GetCurrentProject()
    if not testDir then
        if not res then fail("could not connect to Resolve") end
        if not project then fail("open a project first") end
    end

    local token = tostring(os.time()) .. tostring(math.random(100000, 999999))
    local cmd = 'powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -STA -File "' .. helper
        .. '" -Token ' .. token
    if testDir then cmd = cmd .. ' -Folder "' .. testDir .. '"' end
    log("opening the folder dialog")
    os.execute(cmd)

    local ok, r = pcall(bmd.readfile, resultPath)
    if not ok or type(r) ~= "table" then fail("could not read " .. resultPath .. " (" .. tostring(r) .. ")") end
    if r.Token ~= token then fail("the helper did not finish. See " .. helperDir .. [[\import.log]]) end
    for _, e in ipairs(r.Errors or {}) do log("error: " .. e) end
    if r.Status == "cancelled" then log("no folder selected") return end
    if r.Status == "none" then log("no .360 files in " .. r.Folder) return end
    if r.Status ~= "ok" then fail(table.concat(r.Errors or {}, "; ")) end

    local paths, names = {}, {}
    for i, l in ipairs(r.Links or {}) do
        paths[i] = l.Path
        names[l.Path:lower()] = l.Name
    end
    if testDir then
        for _, p in ipairs(paths) do log("  " .. p) end
        return
    end
    if #paths == 0 then fail("no links could be made in " .. r.Folder .. " (is the drive NTFS?)") end

    local items = project:GetMediaPool():ImportMedia(paths) or {}
    local count = 0
    for _, item in pairs(items) do
        count = count + 1
        local path = item:GetClipProperty("File Path")
        local name = path and names[path:lower()]
        if name then pcall(function() item:SetClipProperty("Clip Name", name) end) end
    end
    log("imported " .. count .. " of " .. #paths .. " .360 file(s) from " .. r.Folder)
    if count == 0 then fail("Resolve imported none of the " .. #paths .. " links in " .. r.Folder) end
end

main()
