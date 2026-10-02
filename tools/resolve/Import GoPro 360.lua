--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360

Resolve will not import files with the .360 extension, so each .360 file gets a second name ending in .mp4 (an NTFS
hard link: same file, no extra disk space) in a hidden "_Max2Reframe" subfolder next to it. This script imports
those links into the current Media Pool bin and renames the clips back to the original file names.

Resolve runs menu scripts in a restricted Lua: no require, io or os.execute, and Fusion's folder dialog does not
open from the Edit page. So the folder dialog and the links come from "Import GoPro 360.ps1" in
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\Max2Reframe. This script starts it through
"Import GoPro 360.cmd" (bmd.openfileexternal) and waits for its result.txt. The helper logs to import.log there.

Failures are raised as Lua errors, so they also appear in Resolve's own log (Support\logs\ResolveDebug.txt).

Test outside Resolve (no dialog, no import):
  set MAX2_TEST_DIR=D:\some\folder
  "C:\Program Files\Blackmagic Design\DaVinci Resolve\fuscript.exe" -l lua "Import GoPro 360.lua"
]]

local helperDir = (os.getenv("APPDATA") or "") .. [[\Blackmagic Design\DaVinci Resolve\Support\Max2Reframe]]
local launcher = helperDir .. [[\Import GoPro 360.cmd]]
local resultPath = helperDir .. [[\result.txt]]
local START_TIMEOUT = 30 -- seconds for the helper to start
local PICK_TIMEOUT = 3600 -- seconds to pick a folder and make the links

local function log(msg) print("Import GoPro 360: " .. tostring(msg)) end

-- Names the functions this Resolve version gives scripts, for the error message.
local function capabilities()
    local names = {}
    for _, n in ipairs({"openfileexternal", "readfile", "fileexists", "wait", "readdir"}) do
        names[#names + 1] = "bmd." .. n .. "=" .. type(bmd and bmd[n])
    end
    names[#names + 1] = "os.time=" .. type(os and os.time)
    return table.concat(names, " ")
end

local function fail(msg) error("Import GoPro 360: " .. msg .. " [" .. capabilities() .. "]", 0) end

local function readResult()
    local ok, r = pcall(bmd.readfile, resultPath)
    if ok and type(r) == "table" then return r end
    return nil
end

local function sleep(seconds)
    if bmd.wait then bmd.wait(seconds) return end
    local t = os.clock() + seconds
    while os.clock() < t do end
end

local function main()
    log("started")
    for _, n in ipairs({"openfileexternal", "readfile", "fileexists"}) do
        if type(bmd[n]) ~= "function" then fail("bmd." .. n .. " is not available in this version of Resolve") end
    end
    if not bmd.fileexists(launcher) then fail("missing " .. launcher .. ". Run Install.bat again.") end

    local testDir = os.getenv("MAX2_TEST_DIR")
    local res = resolve or (bmd.scriptapp and bmd.scriptapp("Resolve"))
    local project = res and res:GetProjectManager():GetCurrentProject()
    if not testDir then
        if not res then fail("could not connect to Resolve") end
        if not project then fail("open a project first") end
    end

    local before = readResult()
    local oldToken = before and before.Token
    log("opening the folder dialog")
    pcall(bmd.openfileexternal, "Open", launcher)

    -- The helper writes result.txt with a new token as soon as it starts, then again when it is done.
    local r
    local waited = 0
    while true do
        sleep(0.25)
        waited = waited + 0.25
        r = readResult()
        local started = r and r.Token ~= oldToken
        if started and r.Status ~= "running" then break end
        if not started and waited > START_TIMEOUT then
            fail("the helper did not start. See " .. helperDir .. [[\import.log]])
        end
        if waited > PICK_TIMEOUT then fail("no folder was selected within " .. PICK_TIMEOUT .. " seconds") end
    end

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
