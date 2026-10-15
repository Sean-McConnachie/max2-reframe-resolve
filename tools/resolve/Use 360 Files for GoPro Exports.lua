--[[
DaVinci Resolve script: Workspace > Scripts > Use 360 Files for GoPro Exports
Replaces GoPro Player exports in the Media Pool with the .360 files they were made from.

"Convert GoPro Reframe to Max2 Reframe" (Max2Convert.exe) converts a timeline's GoPro Reframe clips to Max2 Reframe,
but the converted clips still use the 8K .mp4 exports. Max2 Reframe reads the .360 file next to each export, so the
picture is already right. This script lets you delete the exports: for each export that the converter listed in
%LOCALAPPDATA%\Max2Reframe\exports.lua, it replaces the Media Pool clip with the .360 file's link (made by "Prepare
GoPro 360 for Resolve"). The clip keeps its place in all timelines, with the same trims, because the .360 file has the
same frames and timecode. The script checks this and puts the export back if Resolve reports different values.

The script does not change an export that a timeline still uses with GoPro Reframe: that timeline would then show
the wrong picture. Convert those timelines too, delete the old ones, then run the script again.

A script in Resolve Free cannot show messages. The script writes what it did to the Console (Workspace > Console)
and to %APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\ResolveDebug.txt.
]]

local TITLE = "Use 360 Files for GoPro Exports"
local LINK_DIR = [[\_Max2Reframe\]]
local GOPRO_TOOL = "ofx.com.gopro.plugins.reframe"

local function log(msg) (printerr or print)(TITLE .. ": " .. msg) end

local function folderOf(path) return path:match("^(.*)[\\/][^\\/]*$") or "" end
local function nameOf(path) return path:match("([^\\/]*)$") or path end

-- export path (lower case) -> .360 path, from the converter's list
local function exportList()
    local base = os.getenv("LOCALAPPDATA")
    local path = base and (base .. [[\Max2Reframe\exports.lua]])
    if not path or not bmd.fileexists(path) then return nil, path end
    local ok, list = pcall(dofile, path)
    if not ok or type(list) ~= "table" then
        log("cannot read " .. path .. ": " .. tostring(list))
        return nil, path
    end
    local map = {}
    for _, e in ipairs(list) do
        if type(e.Export) == "string" and type(e.Source) == "string" then map[e.Export:lower()] = e.Source end
    end
    return map, path
end

-- The .mp4 link that "Prepare GoPro 360 for Resolve" made for a .360 file, or nil.
local function linkFor(source)
    local dir = folderOf(source)
    local list = dir .. LINK_DIR .. "files.lua"
    if not bmd.fileexists(list) then return nil end
    local ok, names = pcall(dofile, list)
    if not ok or type(names) ~= "table" then return nil end
    for _, e in ipairs(names) do
        if type(e.Name) == "string" and e.Name:lower() == nameOf(source):lower() then
            local link = e.Link or (dir .. LINK_DIR .. e.Name .. ".mp4")
            if bmd.fileexists(link) then return link end
        end
    end
    return nil
end

-- Unique IDs of Media Pool clips that some timeline still uses with GoPro Reframe, and the timeline names.
local function clipsWithGoPro(project)
    local used = {}
    for i = 1, project:GetTimelineCount() do
        local tl = project:GetTimelineByIndex(i)
        for t = 1, tl:GetTrackCount("video") do
            for _, item in ipairs(tl:GetItemListInTrack("video", t) or {}) do
                local n = item:GetFusionCompCount() or 0
                local gopro = false
                for c = 1, n do
                    local comp = item:GetFusionCompByIndex(c)
                    local tools = comp and comp:GetToolList(false, GOPRO_TOOL)
                    -- if the composition cannot be read, assume that it may use GoPro Reframe
                    if not comp or not tools or next(tools) ~= nil then gopro = true end
                end
                local clip = gopro and item:GetMediaPoolItem()
                if clip then used[clip:GetUniqueId()] = tl:GetName() end
            end
        end
    end
    return used
end

local function allClips(folder, out)
    for _, clip in ipairs(folder:GetClipList() or {}) do out[#out + 1] = clip end
    for _, sub in ipairs(folder:GetSubFolderList() or {}) do allClips(sub, out) end
    return out
end

-- As the Import GoPro 360 scripts: keep only the stereo track of the five that Resolve shows for a .360 file.
local STEREO_ONLY = '{"track_mapping":{"1":{"channel_idx":[1,2],"mute":false,"type":"stereo"}}}'
local function keepStereo(item)
    local ok, mapping = pcall(function() return item:GetAudioMapping() end)
    if not ok or type(mapping) ~= "string" then return end
    local _, tracks = mapping:gsub('"type"', "")
    if tracks == 5 and mapping:find('"1":{"channel_idx":[1,2]', 1, true) then
        if not item:SetAudioMapping(STEREO_ONLY) then log("cannot set the audio of " .. item:GetName()) end
    end
end

local function main()
    local project = resolve:GetProjectManager():GetCurrentProject()
    if not project then return log("open a project first") end
    local exports, listPath = exportList()
    if not exports then
        return log("no list of exports (" .. tostring(listPath) .. "). Convert a timeline with \"Convert GoPro Reframe to Max2 Reframe\" first.")
    end
    local used = clipsWithGoPro(project)
    local replaced, inUse, notPrepared, failed = 0, 0, 0, 0
    for _, clip in ipairs(allClips(project:GetMediaPool():GetRootFolder(), {})) do
        local path = clip:GetClipProperty("File Path")
        local source = type(path) == "string" and exports[path:lower()]
        if source then
            local link = linkFor(source)
            if used[clip:GetUniqueId()] then
                inUse = inUse + 1
                log(clip:GetName() .. ": not changed, timeline \"" .. used[clip:GetUniqueId()] .. "\" uses it with GoPro Reframe")
            elseif not link then
                notPrepared = notPrepared + 1
                log(clip:GetName() .. ": no link for " .. source .. ". In Explorer, right-click its folder > Prepare GoPro 360 for Resolve.")
            else
                local frames, tc = clip:GetClipProperty("Frames"), clip:GetClipProperty("Start TC")
                if not clip:ReplaceClip(link) then
                    failed = failed + 1
                    log(clip:GetName() .. ": Resolve did not replace it with " .. link)
                elseif clip:GetClipProperty("Frames") ~= frames or clip:GetClipProperty("Start TC") ~= tc then
                    failed = failed + 1
                    log(clip:GetName() .. ": the .360 file has other frames or timecode (" .. tostring(clip:GetClipProperty("Frames")) ..
                        " frames from " .. tostring(clip:GetClipProperty("Start TC")) .. "); the export is back in place")
                    clip:ReplaceClip(path)
                else
                    replaced = replaced + 1
                    local name = nameOf(source)
                    if not (clip.SetName and clip:SetName(name)) then clip:SetClipProperty("Clip Name", name) end
                    keepStereo(clip)
                    log(name .. ": now uses " .. link)
                end
            end
        end
    end
    log(replaced .. " clip(s) now use their .360 file; " .. inUse .. " still used with GoPro Reframe; " .. notPrepared ..
        " without a prepared .360 file; " .. failed .. " failed")
end

main()
