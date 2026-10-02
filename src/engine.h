// Shared, cached access to .360 clips (metadata + decoders) and the full render settings.
#pragma once

#include <memory>
#include <string>

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

class Clip360
{
public:
    // Opens (or returns the cached) clip. Returns null and sets err on failure.
    static std::shared_ptr<Clip360> open(const std::wstring& path, std::string* err);

    const Source360Info& info() const { return m_Info; }
    // Decode both lens streams for a frame (in parallel).
    bool fetch(int frame, std::shared_ptr<const Nv12Frame>& s0, std::shared_ptr<const Nv12Frame>& s1, std::string* err);
    ViewParams view(int frame, const RenderSettings& rs);
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
