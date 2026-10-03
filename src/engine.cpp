#include "engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <list>
#include <map>
#include <mutex>

#include "log.h"

namespace {

constexpr size_t kMaxOpenClips = 3;

struct Registry
{
    std::mutex mutex;
    std::list<std::pair<std::wstring, std::shared_ptr<Clip360>>> clips;  // most recently used first
    std::map<std::wstring, std::pair<std::string, std::chrono::steady_clock::time_point>> failures;
};

Registry& registry()
{
    static Registry* r = new Registry();  // intentionally leaked: avoid teardown order issues at DLL unload
    return *r;
}

std::wstring normalize(const std::wstring& p)
{
    std::wstring s = p;
    std::replace(s.begin(), s.end(), L'/', L'\\');
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

} // namespace

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::shared_ptr<Clip360> Clip360::open(const std::wstring& path, std::string* err)
{
    Registry& r = registry();
    std::wstring key = normalize(path);
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        for (auto it = r.clips.begin(); it != r.clips.end(); ++it)
            if (it->first == key)
            {
                r.clips.splice(r.clips.begin(), r.clips, it);
                return it->second;
            }
        auto f = r.failures.find(key);
        if (f != r.failures.end() && std::chrono::steady_clock::now() - f->second.second < std::chrono::seconds(10))
        {
            if (err) *err = f->second.first;
            return nullptr;
        }
    }
    Source360Info info;
    std::string e;
    if (!loadSource360(path, info, &e))
    {
        logf("open %s: %s", wideToUtf8(path).c_str(), e.c_str());
        std::lock_guard<std::mutex> lock(r.mutex);
        r.failures[key] = {e, std::chrono::steady_clock::now()};
        if (err) *err = e;
        return nullptr;
    }
    logf("open %s: %d frames @ %d/%d fps, streams %dx%d %d-bit, tc start %lld, CORI %zu IORI %zu GRAV %zu",
         wideToUtf8(path).c_str(), info.frames, info.fpsNum, info.fpsDen, info.streamW, info.streamH, info.bitDepth,
         (long long)info.tcStartFrame, info.cori.size(), info.iori.size(), info.grav.size());
    auto clip = std::make_shared<Clip360>(info);
    std::lock_guard<std::mutex> lock(r.mutex);
    for (auto& c : r.clips)
        if (c.first == key) return c.second;  // another thread won the race
    r.clips.emplace_front(key, clip);
    while (r.clips.size() > kMaxOpenClips) r.clips.pop_back();  // decoders close when the last user lets go
    return clip;
}

Clip360::Clip360(const Source360Info& info) : m_Info(info), m_Stab(info)
{
    for (int i = 0; i < 2; ++i)
        m_Dec[i] = std::make_unique<StreamDecoder>(info.path, info.videoTrackIds[i], info.videoFirstSampleSizes[i], i,
                                                 info.fpsNum, info.fpsDen, info.frames, info.bitDepth);
}

bool Clip360::fetch(int frame, std::shared_ptr<const Nv12Frame>& s0, std::shared_ptr<const Nv12Frame>& s1, std::string* err)
{
    frame = std::clamp(frame, 0, std::max(0, m_Info.frames - 1));
    m_Dec[0]->request(frame);
    m_Dec[1]->request(frame);
    s0 = m_Dec[0]->get(frame);
    s1 = m_Dec[1]->get(frame);
    if (s0 && s1) return true;
    if (err) *err = !s0 ? m_Dec[0]->error() : m_Dec[1]->error();
    return false;
}

ViewParams Clip360::view(int frame, const RenderSettings& rs, double subFrame)
{
    ViewParams vp;
    Mat3 user = Mat3::rotY(deg2rad(rs.pan)) * Mat3::rotX(deg2rad(-rs.tilt)) * Mat3::rotZ(deg2rad(-rs.roll));
    vp.R = m_Stab.rotation(frame, rs.stab, subFrame) * user;
    vp.proj = rs.proj;
    vp.fovH = deg2rad(rs.fovDeg);
    vp.curvature = rs.curvature;
    vp.supersample = rs.supersample;
    return vp;
}

int resolveSourceFrame(const Source360Info& info, double hostFrame)
{
    long long f = (long long)std::floor(hostFrame + 0.5);
    if (f >= info.frames && info.tcStartFrame > 0 && f >= info.tcStartFrame) f -= info.tcStartFrame;
    return int(std::clamp<long long>(f, 0, std::max(0, info.frames - 1)));
}

namespace {

// Longest on-screen distance (pixels) any of a few probe points moves between two views.
double estimateBlurPx(const RfParams& a, const RfParams& b)
{
    const float W = float(a.outW), H = float(a.outH);
    const float probes[][2] = {{0.5f, 0.5f}, {0.05f, 0.5f}, {0.95f, 0.5f}, {0.5f, 0.05f}, {0.5f, 0.95f},
                               {0.05f, 0.05f}, {0.95f, 0.05f}, {0.05f, 0.95f}, {0.95f, 0.95f}};
    // pixels per radian near the centre of the view (every lens is ~equidistant at the centre)
    double scale = a.proj == 1 ? W / (2 * kPi) : (W * 0.5) / std::max(1e-3, double(a.rhoScale));
    double worst = 0;
    for (const auto& pr : probes)
    {
        float va[3], vb[3];
        if (!rf_view_dir(&a, pr[0] * W, pr[1] * H, va) || !rf_view_dir(&b, pr[0] * W, pr[1] * H, vb)) continue;
        double da[3], db[3];
        for (int i = 0; i < 3; ++i)
        {
            da[i] = a.R[i * 3] * va[0] + a.R[i * 3 + 1] * va[1] + a.R[i * 3 + 2] * va[2];
            db[i] = b.R[i * 3] * vb[0] + b.R[i * 3 + 1] * vb[1] + b.R[i * 3 + 2] * vb[2];
        }
        double d = std::clamp(da[0] * db[0] + da[1] * db[1] + da[2] * db[2], -1.0, 1.0);
        worst = std::max(worst, std::acos(d));
    }
    return worst * scale;
}

} // namespace

std::vector<RfParams> buildRenderSteps(Clip360& clip, int frame, const std::function<RenderSettings(double)>& settingsAt,
                                       const MotionBlurSettings& mb, const Nv12Frame& stream, int outW, int outH,
                                       int outStride, double* blurPx)
{
    auto at = [&](double dt) {
        return makeRfParams(clip.view(frame, settingsAt(dt), dt), stream, outW, outH, outStride);
    };
    if (blurPx) *blurPx = 0;
    double shutter = std::clamp(mb.shutterAngle, 0.0, 360.0) / 360.0;
    if (!mb.enabled || shutter <= 0) return {at(0)};

    double blur = estimateBlurPx(at(-shutter / 2), at(shutter / 2));
    if (blurPx) *blurPx = blur;
    if (blur < 0.5) return {at(0)};
    int n = std::clamp(int(std::ceil(blur)), 2, std::max(2, mb.maxSamples));

    std::vector<RfParams> steps;
    steps.reserve(n);
    for (int k = 0; k < n; ++k)
    {
        double dt = ((k + 0.5) / n - 0.5) * shutter;
        RenderSettings rs = settingsAt(dt);
        if (n >= 4) rs.supersample = 1;  // the time samples are jittered, so they antialias as well
        RfParams p = makeRfParams(clip.view(frame, rs, dt), stream, outW, outH, outStride);
        if (n >= 4)
        {
            // R2 low-discrepancy sequence for the sub-pixel jitter
            p.jitterX = float(std::fmod(0.5 + k * 0.7548776662466927, 1.0) - 0.5);
            p.jitterY = float(std::fmod(0.5 + k * 0.5698402909980532, 1.0) - 0.5);
        }
        steps.push_back(p);
    }
    return steps;
}
