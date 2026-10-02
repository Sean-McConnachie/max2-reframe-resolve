// GoPro .360 file info: video stream layout, timing and the GPMF orientation data used for stabilization.
#pragma once

#include <array>
#include <map>
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
    int videoTracks = 0;
    std::vector<uint32_t> videoTrackIds;  // MP4 track IDs of the video tracks, in file order
    std::vector<uint32_t> videoFirstSampleSizes;
    std::vector<std::array<double, 4>> cori, iori;  // w, x, y, z per frame
    std::vector<Vec3> grav;
};

bool loadSource360(const std::wstring& path, Source360Info& info, std::string* err);

struct StabSettings
{
    bool stabilize = true;
    bool horizon = true;
    bool directionLock = false;
    double smoothSeconds = 0.3;   // heading / orientation smoothing (Gaussian sigma)
    double gravitySeconds = 3.0;  // horizon (gravity) smoothing

    bool operator<(const StabSettings& o) const
    {
        return std::tie(stabilize, horizon, directionLock, smoothSeconds, gravitySeconds) <
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
    const std::vector<Mat3>& table(const StabSettings& s);

    double m_Fps;
    std::vector<Mat3> m_T;     // world0 -> camera
    std::vector<Vec3> m_Grav;  // gravity (up) in world0, per frame
    std::mutex m_Mutex;
    std::map<StabSettings, std::vector<Mat3>> m_Cache;  // per frame: output view -> world0
};
