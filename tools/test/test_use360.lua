--[[
Test for "Use 360 Files for GoPro Exports.lua" with a fake Resolve. test_convert.py runs it with Resolve's script
interpreter, with the script's path in the environment variable USE360_SCRIPT:
    fuscript.exe -l lua tools\test\test_use360.lua
Prints "ok"/"FAIL" lines and "failures N".
]]
local script = os.getenv("USE360_SCRIPT")
local realDofile = dofile

local files, exists, logs, failures = {}, {}, {}, 0
local function check(cond, what)
    print((cond and "  ok    " or "  FAIL  ") .. what)
    if not cond then failures = failures + 1 end
end

-- Resolve's interpreter protects its own globals, so set them with rawset
rawset(_G, "printerr", function(m) logs[#logs + 1] = m end)
rawset(_G, "bmd", { fileexists = function(p) return exists[p] == true end })
rawset(_G, "dofile", function(p)
    if files[p] == nil then error("no file " .. tostring(p)) end
    return files[p]
end)

local LA = os.getenv("LOCALAPPDATA")
local list = LA .. [[\Max2Reframe\exports.lua]]
files[list] = {
    { Export = [[E:\Trip\equi\A.mp4]], Source = [[E:\Trip\MAX2\A.360]] },
    { Export = [[E:\Trip\equi\B.mp4]], Source = [[E:\Trip\MAX2\B.360]] },
    { Export = [[E:\Trip\equi\C.mp4]], Source = [[E:\Other\C.360]] },
    { Export = [[E:\Trip\equi\D.mp4]], Source = [[E:\Trip\MAX2\D.360]] },
    { Export = [[E:\Trip\equi\F.mp4]], Source = [[E:\Trip\MAX2\F.360]] },
}
exists[list] = true
local prepared = [[E:\Trip\MAX2\_Max2Reframe\files.lua]]
files[prepared] = { { Name = "A.360" }, { Name = "B.360" }, { Name = "D.360" }, { Name = "F.360", Link = [[C:\Links\E\Trip\MAX2\F.360.mp4]] } }
exists[prepared] = true
for _, p in ipairs({ [[E:\Trip\MAX2\_Max2Reframe\A.360.mp4]], [[E:\Trip\MAX2\_Max2Reframe\B.360.mp4]],
                     [[E:\Trip\MAX2\_Max2Reframe\D.360.mp4]], [[C:\Links\E\Trip\MAX2\F.360.mp4]] }) do
    exists[p] = true
end

-- what Resolve would report for a file after ReplaceClip
local probe = {
    [ [[E:\Trip\MAX2\_Max2Reframe\A.360.mp4]] ] = { "945", "13:02:30:28" },
    [ [[C:\Links\E\Trip\MAX2\F.360.mp4]] ] = { "650", "13:05:50:28" },
    [ [[E:\Trip\MAX2\_Max2Reframe\D.360.mp4]] ] = { "900", "13:31:27:28" },  -- not the same as the export
}

local function Clip(id, name, path, frames, tc)
    local c = { id = id, name = name, path = path, frames = frames, tc = tc, replaced = {}, audio = nil }
    function c:GetUniqueId() return self.id end
    function c:GetName() return self.name end
    function c:SetName(n) self.name = n; return true end
    function c:GetClipProperty(k)
        if k == "File Path" then return self.path end
        if k == "Frames" then return self.frames end
        if k == "Start TC" then return self.tc end
    end
    function c:ReplaceClip(p)
        self.replaced[#self.replaced + 1] = p
        local v = probe[p]
        self.path = p
        if v then self.frames, self.tc = v[1], v[2] end
        return true
    end
    function c:GetAudioMapping()
        return '{"embedded_audio_channels":6,"linked_audio":{},"track_mapping":{"1":{"channel_idx":[1,2],"mute":false,"type":"stereo"},' ..
            '"2":{"channel_idx":[3],"mute":false,"type":"mono"},"3":{"channel_idx":[4],"mute":false,"type":"mono"},' ..
            '"4":{"channel_idx":[5],"mute":false,"type":"mono"},"5":{"channel_idx":[6],"mute":false,"type":"mono"}}}'
    end
    function c:SetAudioMapping(m) self.audio = m; return true end
    return c
end

local A = Clip("a", "A.mp4", [[E:\Trip\equi\A.mp4]], "945", "13:02:30:28")
local B = Clip("b", "B.mp4", [[E:\Trip\equi\B.mp4]], "100", "10:00:00:00")
local C = Clip("c", "C.mp4", [[E:\Trip\equi\C.mp4]], "100", "10:00:00:00")
local D = Clip("d", "D.mp4", [[E:\Trip\equi\D.mp4]], "1428", "13:31:27:28")
local E = Clip("e", "phone.mp4", [[E:\Trip\phone\phone.mp4]], "300", "00:00:00:00")
local F = Clip("f", "F.mp4", [[E:\TRIP\EQUI\F.MP4]], "650", "13:05:50:28")  -- the Media Pool can spell the path differently

local function Folder(clips, subs)
    return { GetClipList = function() return clips end, GetSubFolderList = function() return subs or {} end }
end
local root = Folder({ A, B }, { Folder({ C, D }, { Folder({ E, F }) }) })

local function Comp(tools) return { GetToolList = function(_, sel, t) return t == "ofx.com.gopro.plugins.reframe" and tools or {} end } end
local function Item(clip, comps)
    return {
        GetMediaPoolItem = function() return clip end,
        GetFusionCompCount = function() return #comps end,
        GetFusionCompByIndex = function(_, i) return comps[i] end,
    }
end
local function Timeline(name, items)
    return {
        GetName = function() return name end,
        GetTrackCount = function(_, kind) return kind == "video" and 1 or 0 end,
        GetItemListInTrack = function(_, kind, i) return items end,
    }
end
local timelines = {
    Timeline("Old", { Item(B, { Comp({ GoProReframe1 = {} }) }), Item(A, { Comp({}) }) }),  -- A only has another composition
    Timeline("New (Max2 Reframe)", { Item(A, {}), Item(D, {}), Item(F, {}) }),
}
local project = {
    GetMediaPool = function() return { GetRootFolder = function() return root end } end,
    GetTimelineCount = function() return #timelines end,
    GetTimelineByIndex = function(_, i) return timelines[i] end,
}
rawset(_G, "resolve", { GetProjectManager = function() return { GetCurrentProject = function() return project end } end })

realDofile(script)

print("Use 360 Files for GoPro Exports (fake Resolve)")
check(A.path == [[E:\Trip\MAX2\_Max2Reframe\A.360.mp4]] and A.name == "A.360", "export replaced with its hard link, clip named after the .360 file")
check(A.audio and A.audio:find('"type":"stereo"') and not A.audio:find("mono"), "only the stereo audio track is kept")
check(F.path == [[C:\Links\E\Trip\MAX2\F.360.mp4]], "export replaced with its symbolic link from files.lua (path compared without case)")
check(#B.replaced == 0, "export that a timeline uses with GoPro Reframe is not changed")
check(#C.replaced == 0, "export whose .360 folder is not prepared is not changed")
check(#D.replaced == 2 and D.path == [[E:\Trip\equi\D.mp4]] and D.name == "D.mp4", "export put back when the .360 file reports other frames")
check(#E.replaced == 0, "clip that is not in the list is not touched")
local all = table.concat(logs, "\n")
check(all:find("2 clip%(s%) now use their .360 file; 1 still used with GoPro Reframe; 1 without a prepared .360 file; 1 failed") ~= nil,
      "summary line")
check(all:find('timeline "Old" uses it with GoPro Reframe', 1, true) ~= nil, "the log names the timeline that still uses GoPro Reframe")
if failures > 0 then print("script log:\n  " .. table.concat(logs, "\n  ")) end
print("failures " .. failures)
