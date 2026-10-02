// Shared, cached access to .360 clips (metadata + decoders) and the full render settings.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "decoder.h"
#include "reproject.h"
#include "source360.h"

struct RenderSettings
{
    StabSettings stab;
    double pan = 0, tilt = 0, roll = 0;  // degrees
    Projection proj = Projection::Lens;
    double fovDeg = 100;
    double curvature = 0.4;
    int supersample = 1;
};

struct MotionBlurSettings
{
    bool enabled = false;
    double shutterAngle = 180;  // degrees; 360 = the whole frame interval
    int maxSamples = 32;
};

class Clip360;

// Kernel parameter sets for one output frame: a single set without motion blur, otherwise one per time sample
// across the shutter interval (centred on the frame). settingsAt(dt) returns the settings dt frames from the
// frame being rendered, so animated parameters are blurred too. blurPx receives the estimated blur length.
std::vector<RfParams> buildRenderSteps(Clip360& clip, int frame, const std::function<RenderSettings(double)>& settingsAt,
                                       const MotionBlurSettings& mb, const Nv12Frame& stream, int outW, int outH,
                                       int outStride, double* blurPx = nullptr);

class Clip360
{
public:
    // Opens (or returns the cached) clip. Returns null and sets err on failure.
    static std::shared_ptr<Clip360> open(const std::wstring& path, std::string* err);

    const Source360Info& info() const { return m_Info; }
    // Decode both lens streams for a frame (in parallel).
    bool fetch(int frame, std::shared_ptr<const Nv12Frame>& s0, std::shared_ptr<const Nv12Frame>& s1, std::string* err);
    // subFrame: time offset of the virtual camera within the frame, in frames (for motion blur).
    ViewParams view(int frame, const RenderSettings& rs, double subFrame = 0);
    bool hasGyro() const { return m_Stab.hasData(); }

    explicit Clip360(const Source360Info& info);

private:
    Source360Info m_Info;
    Stabilizer m_Stab;
    std::unique_ptr<StreamDecoder> m_Dec[2];
};

// Map a frame number reported by the host to a 0-based frame in the file (handles timecode-based numbers).
int resolveSourceFrame(const Source360Info& info, double hostFrame);

std::wstring utf8ToWide(const std::string& s);
std::string wideToUtf8(const std::wstring& s);
