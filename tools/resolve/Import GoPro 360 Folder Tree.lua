--[[
DaVinci Resolve script: Workspace > Scripts > Import GoPro 360 Folder Tree
Imports the .360 files of a folder and all its subfolders. The folder becomes a bin in the current Media Pool bin,
and each subfolder with .360 files becomes a bin inside it, so the bins have the same structure as the folders.

First, in Explorer, right-click the folder > "Prepare GoPro 360 for Resolve" (Max2Prepare.exe). That makes a hidden
"_Max2Reframe" folder with a files.lua list of .mp4 links in each folder with .360 files (Resolve 21.1 Free
does not import .360 files, and its menu scripts cannot create files or start programs). This script finds the
subfolders with Resolve's Media Storage, imports the links and names the clips after the .360 files. Existing bins
are reused and clips that are already in a bin are skipped, so the script can run again after more files are added.

Messages appear as the title of the folder dialog. Details go to
%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\ResolveDebug.txt.
]]

local TITLE = "Import GoPro 360 Folder Tree"
local LINK_DIR = [[\_Max2Reframe\]]
local MAX_FOLDERS = 20000 -- stop scanning a very large tree (for example a whole drive)

local function log(msg) (printerr or print)(TITLE .. ": " .. msg) end

local function leaf(dir) return dir:match("([^\\/]+)$") or dir end

local function pickFolder(title, start)
    local dir = fusion:RequestDir(start, { FReqS_Title = title })
    if type(dir) ~= "string" or dir == "" then return nil end
    return (dir:gsub("[\\/]+$", ""))
end

local function preparedFiles(dir)
    local path = dir .. LINK_DIR .. "files.lua"
    if not bmd.fileexists(path) then return {} end
    local ok, names = pcall(dofile, path)
    if not ok or type(names) ~= "table" then
        log("cannot read " .. path .. ": " .. tostring(names))
        return {}
    end
    return names
end

-- Returns { dir, names, children } for dir, or nil if dir and its subfolders have no prepared files.
local function scan(storage, dir, state)
    state.folders = state.folders + 1
    if state.folders > MAX_FOLDERS then
        state.truncated = true
        return nil
    end
    local node = { dir = dir, names = preparedFiles(dir), children = {} }
    local subs = {}
    for _, sub in ipairs(storage:GetSubFolderList(dir) or {}) do
        if type(sub) == "string" and leaf(sub):lower() ~= "_max2reframe" then subs[#subs + 1] = sub end
    end
    table.sort(subs, function(a, b) return a:lower() < b:lower() end)
    for _, sub in ipairs(subs) do
        local child = scan(storage, (sub:gsub("[\\/]+$", "")), state)
        if child then node.children[#node.children + 1] = child end
    end
    if #node.names == 0 and #node.children == 0 then return nil end
    return node
end

-- The bin called name inside parent; made if missing.
local function subBin(mediaPool, parent, name)
    for _, f in ipairs(parent:GetSubFolderList() or {}) do
        if f:GetName() == name then return f end
    end
    return mediaPool:AddSubFolder(parent, name)
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

local function importTree(mediaPool, parentBin, node, totals)
    local bin = subBin(mediaPool, parentBin, (leaf(node.dir):gsub(":$", "")))
    if not bin then
        log("cannot make a bin for " .. node.dir)
        return
    end
    if #node.names > 0 then
        local count, already = importFolder(mediaPool, bin, node.dir, node.names)
        totals.imported = totals.imported + count
        totals.already = totals.already + already
    end
    for _, child in ipairs(node.children) do importTree(mediaPool, bin, child, totals) end
end

local function main()
    local project = resolve:GetProjectManager():GetCurrentProject()
    if not project then return log("open a project first") end
    local mediaPool = project:GetMediaPool()
    local storage = resolve:GetMediaStorage()
    local startBin = mediaPool:GetCurrentFolder()

    local title = TITLE .. ": select the top folder (prepared in Explorer)"
    local start = fusion:GetData("Max2Reframe.LastFolder")
    while true do
        local dir = pickFolder(title, start)
        if not dir then return end
        start = dir
        fusion:SetData("Max2Reframe.LastFolder", dir)
        local state = { folders = 0 }
        local tree = scan(storage, dir, state)
        if state.truncated then log("stopped after " .. MAX_FOLDERS .. " folders in " .. dir) end
        if not tree then
            title = "Not prepared: in Explorer, right-click the folder > Prepare GoPro 360 for Resolve. Then select it again."
        else
            local totals = { imported = 0, already = 0 }
            importTree(mediaPool, startBin, tree, totals)
            mediaPool:SetCurrentFolder(startBin)
            log("imported " .. totals.imported .. " clip(s) from " .. dir .. ", " .. totals.already .. " already in bins")
            if totals.imported > 0 then return end
            if totals.already > 0 then
                title = "All " .. totals.already .. " clips are already in the bins. Select another folder or Cancel."
            else
                title = "Resolve imported none of the files. Prepare the folder again in Explorer, then select it."
            end
        end
    end
end

main()
