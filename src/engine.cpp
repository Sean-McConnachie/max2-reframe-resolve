#include "engine.h"

#include <windows.h>

#include <algorithm>
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
    logf("open %s: %d frames @ %d/%d fps, streams %dx%d, tc start %lld, CORI %zu IORI %zu GRAV %zu",
         wideToUtf8(path).c_str(), info.frames, info.fpsNum, info.fpsDen, info.streamW, info.streamH,
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
                                                 info.fpsNum, info.fpsDen, info.frames);
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

ViewParams Clip360::view(int frame, const RenderSettings& rs)
{
    ViewParams vp;
    Mat3 user = Mat3::rotY(deg2rad(rs.pan)) * Mat3::rotX(deg2rad(-rs.tilt)) * Mat3::rotZ(deg2rad(-rs.roll));
    vp.R = m_Stab.rotation(frame, rs.stab) * user;
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
