--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360
Imports the .360 files of one folder (not its subfolders) into the current Media Pool bin.

Resolve does not import files with the .360 extension, and in Resolve 21.1 Free a menu script cannot create files or
start programs. So first, in Explorer, right-click the folder > "Prepare GoPro 360 for Resolve" (Max2Prepare.exe).
That makes an .mp4 link for each .360 file and a files.lua list in a hidden "_Max2Reframe" folder. This script
reads the list, imports the links and names the clips after the .360 files. Clips that are already in the bin
are skipped, so the script can run again after more files are added and prepared.

A script cannot show a message box either, so messages appear as the title of the folder dialog. Details go to
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\ResolveDebug.txt.
]]

local TITLE = "Import GoPro 360"
local LINK_DIR = [[\_Max2Reframe\]]

local function log(msg) (printerr or print)(TITLE .. ": " .. msg) end

-- The folder from the dialog, without the trailing backslash ("D:\" becomes "D:"). nil if cancelled.
local function pickFolder(title, start)
    local dir = fusion:RequestDir(start, { FReqS_Title = title })
    if type(dir) ~= "string" or dir == "" then return nil end
    return (dir:gsub("[\\/]+$", ""))
end

-- Names of the prepared .360 files in dir, or nil if the folder was not prepared.
local function preparedFiles(dir)
    local path = dir .. LINK_DIR .. "files.lua"
    if not bmd.fileexists(path) then return nil end
    local ok, names = pcall(dofile, path)
    if not ok or type(names) ~= "table" then
        log("cannot read " .. path .. ": " .. tostring(names))
        return nil
    end
    return names
end

-- A Max 2 file has a stereo AAC stream and a 4-channel ambisonic stream, which Resolve shows as 1 stereo and 4 mono
-- audio tracks. Keep only the stereo track, unless someone changed the clip's audio mapping by hand.
local STEREO_ONLY = '{"track_mapping":{"1":{"channel_idx":[1,2],"mute":false,"type":"stereo"}}}'
local function keepStereo(item)
    local ok, mapping = pcall(function() return item:GetAudioMapping() end)
    if not ok or type(mapping) ~= "string" then return end
    local _, tracks = mapping:gsub('"type"', "")
    if tracks == 5 and mapping:find('"1":{"channel_idx":[1,2]', 1, true) then
        if not item:SetAudioMapping(STEREO_ONLY) then log("cannot set the audio of " .. item:GetName()) end
    end
end

-- Imports the prepared files of dir into bin. Returns the number imported and the number already there.
local function importFolder(mediaPool, bin, dir, names)
    local have = {}
    for _, clip in ipairs(bin:GetClipList() or {}) do
        local p = clip:GetClipProperty("File Path")
        if type(p) == "string" then have[p:lower()] = clip end
    end
    local paths, nameOf, already = {}, {}, 0
    for _, entry in ipairs(names) do
        local name = entry.Name
        -- a symbolic link elsewhere (drives without hard links), or a hard link in the _Max2Reframe folder
        local link = entry.Link or (dir .. LINK_DIR .. name .. ".mp4")
        if have[link:lower()] then
            already = already + 1
            keepStereo(have[link:lower()]) -- clips imported by an earlier version of this script
        elseif bmd.fileexists(link) then
            paths[#paths + 1] = link
            nameOf[link:lower()] = name
        else
            log("missing " .. link .. ". Prepare the folder again.")
        end
    end
    if #paths == 0 then return 0, already end
    mediaPool:SetCurrentFolder(bin)
    local count = 0
    for _, item in ipairs(mediaPool:ImportMedia(paths) or {}) do
        count = count + 1
        local p = item:GetClipProperty("File Path")
        local name = type(p) == "string" and nameOf[p:lower()]
        if name and not (item.SetName and item:SetName(name)) then item:SetClipProperty("Clip Name", name) end
        keepStereo(item)
    end
    if count < #paths then log("Resolve imported " .. count .. " of " .. #paths .. " files from " .. dir) end
    return count, already
end

local function main()
    local project = resolve:GetProjectManager():GetCurrentProject()
    if not project then return log("open a project first") end
    local mediaPool = project:GetMediaPool()
    local bin = mediaPool:GetCurrentFolder()

    local title = TITLE .. ": select a folder with .360 files (prepared in Explorer)"
    local start = fusion:GetData("Max2Reframe.LastFolder")
    while true do
        local dir = pickFolder(title, start)
        if not dir then return end
        start = dir
        fusion:SetData("Max2Reframe.LastFolder", dir)
        local names = preparedFiles(dir)
        if not names or #names == 0 then
            title = "Not prepared: in Explorer, right-click the folder > Prepare GoPro 360 for Resolve. Then select it again."
        else
            local count, already = importFolder(mediaPool, bin, dir, names)
            log("imported " .. count .. " clip(s) from " .. dir .. ", " .. already .. " already in the bin")
            if count > 0 then return end
            if already > 0 then
                title = "All " .. already .. " clips of this folder are already in the bin. Select another folder or Cancel."
            else
                title = "Resolve imported none of the files. Prepare the folder again in Explorer, then select it."
            end
        end
    end
end

main()
