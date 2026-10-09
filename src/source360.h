// GoPro .360 file info: video stream layout, timing and the GPMF orientation data used for stabilization.
#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "mathx.h"

struct Source360Info
{
    std::wstring path;
    int frames = 0;             // video frame count
    int fpsNum = 30000, fpsDen = 1001;
    double fps() const { return double(fpsNum) / fpsDen; }
    int64_t tcStartFrame = -1;  // timecode of first frame as a frame count, -1 if none
    int streamW = 0, streamH = 0;
    int bitDepth = 8;           // 10 for 10-bit recordings (GP-Log)
    int videoTracks = 0;
    std::vector<uint32_t> videoTrackIds;  // MP4 track IDs of the video tracks, in file order
    std::vector<uint32_t> videoFirstSampleSizes;
    std::vector<std::array<double, 4>> cori, iori;  // w, x, y, z per frame
    std::vector<Vec3> grav;
    std::vector<int> keyframes;  // 0-based frames that are keyframes, in order; empty means all frames
    // The last keyframe at or before `frame`: the frame that is quickest to decode near it.
    int keyframeAtOrBefore(int frame) const;
};

bool loadSource360(const std::wstring& path, Source360Info& info, std::string* err);

struct StabSettings
{
    bool stabilize = true;
    bool horizon = true;
    bool directionLock = false;
    double smoothSeconds = 0.3;   // heading / orientation smoothing (Gaussian sigma)
    double gravitySeconds = 3.0;  // horizon (gravity) smoothing

    bool operator==(const StabSettings& o) const
    {
        return std::tie(stabilize, horizon, directionLock, smoothSeconds, gravitySeconds) ==
               std::tie(o.stabilize, o.horizon, o.directionLock, o.smoothSeconds, o.gravitySeconds);
    }
};

// Computes the rotation mapping output view directions to camera directions.
class Stabilizer
{
public:
    explicit Stabilizer(const Source360Info& info);
    // subFrame offsets the virtual camera in time (for motion blur); the source frame stays `frame`.
    Mat3 rotation(int frame, const StabSettings& s, double subFrame = 0);
    bool hasData() const { return !m_T.empty(); }

private:
    using Table = std::shared_ptr<const std::vector<Mat3>>;
    Table table(StabSettings s);

    double m_Fps;
    std::vector<Mat3> m_T;     // world0 -> camera
    std::vector<Vec3> m_Grav;  // gravity (up) in world0, per frame
    std::mutex m_Mutex;
    // Per frame: output view -> world0. Most recently used first. An animated Smoothing gives a new table
    // for each value, so only a few stay.
    std::vector<std::pair<StabSettings, Table>> m_Cache;
};
