#include "source360.h"

#include <algorithm>
#include <cstring>
#include <tuple>

#include "mp4.h"

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Decode one numeric element of GPMF type t at p.
double gpmfValue(char t, const uint8_t* p)
{
    switch (t)
    {
    case 'b': return double(int8_t(p[0]));
    case 'B': return double(p[0]);
    case 's': return double(int16_t((p[0] << 8) | p[1]));
    case 'S': return double(uint16_t((p[0] << 8) | p[1]));
    case 'l': return double(int32_t(be32(p)));
    case 'L': return double(be32(p));
    case 'f': { uint32_t u = be32(p); float f; std::memcpy(&f, &u, 4); return f; }
    case 'd': { uint64_t u = (uint64_t(be32(p)) << 32) | be32(p + 4); double d; std::memcpy(&d, &u, 8); return d; }
    default: return 0;
    }
}

int gpmfSize(char t)
{
    switch (t)
    {
    case 'b': case 'B': return 1;
    case 's': case 'S': return 2;
    case 'l': case 'L': case 'f': return 4;
    case 'd': return 8;
    default: return 0;
    }
}

struct GpmfOut
{
    std::vector<std::array<double, 4>> cori, iori;
    std::vector<Vec3> grav;
};

// Walk a STRM's KLVs, tracking SCAL, and collect the keys we care about.
void parseStrm(const uint8_t* p, size_t n, GpmfOut& out)
{
    std::vector<double> scal;
    size_t off = 0;
    while (off + 8 <= n)
    {
        char key[5] = {char(p[off]), char(p[off + 1]), char(p[off + 2]), char(p[off + 3]), 0};
        char type = char(p[off + 4]);
        int ssize = p[off + 5];
        int rep = (p[off + 6] << 8) | p[off + 7];
        size_t len = size_t(ssize) * rep;
        const uint8_t* d = p + off + 8;
        if (off + 8 + len > n) break;
        int es = gpmfSize(type);
        if (std::strcmp(key, "SCAL") == 0 && es)
        {
            scal.clear();
            for (size_t i = 0; i + es <= len; i += es) scal.push_back(gpmfValue(type, d + i));
        }
        else if (es && (!std::strcmp(key, "CORI") || !std::strcmp(key, "IORI") || !std::strcmp(key, "GRAV")))
        {
            int comps = ssize / es;
            for (int r = 0; r < rep; ++r)
            {
                double v[4] = {0, 0, 0, 0};
                for (int c = 0; c < comps && c < 4; ++c)
                {
                    double s = scal.empty() ? 1.0 : (scal.size() == size_t(comps) ? scal[c] : scal[0]);
                    v[c] = gpmfValue(type, d + size_t(r) * ssize + size_t(c) * es) / (s != 0 ? s : 1.0);
                }
                if (key[0] == 'C') out.cori.push_back({v[0], v[1], v[2], v[3]});
                else if (key[0] == 'I') out.iori.push_back({v[0], v[1], v[2], v[3]});
                else out.grav.emplace_back(v[0], v[1], v[2]);
            }
        }
        off += 8 + ((len + 3) & ~size_t(3));
    }
}

void parseGpmf(const uint8_t* p, size_t n, GpmfOut& out)
{
    size_t off = 0;
    while (off + 8 <= n)
    {
        char type = char(p[off + 4]);
        size_t len = size_t(p[off + 5]) * ((p[off + 6] << 8) | p[off + 7]);
        if (off + 8 + len > n) break;
        if (type == 0)
        {
            if (std::memcmp(p + off, "STRM", 4) == 0) parseStrm(p + off + 8, len, out);
            else parseGpmf(p + off + 8, len, out);  // DEVC and other containers
        }
        off += 8 + ((len + 3) & ~size_t(3));
    }
}

// Map frame i of n to an index in a per-frame metadata array of size m.
size_t metaIndex(int i, int n, size_t m)
{
    if (m == 0) return 0;
    if (n <= 1 || size_t(n) == m) return std::min(size_t(std::max(i, 0)), m - 1);
    double t = double(i) * double(m) / double(n);
    return std::min(size_t(std::max(0.0, t)), m - 1);
}

// scipy.ndimage 'mirror' boundary: d c b | a b c d | c b a
int mirrorIndex(int i, int n)
{
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    i = std::abs(i) % period;
    return i >= n ? period - i : i;
}

std::vector<Vec3> gaussSmooth(const std::vector<Vec3>& v, double sigma)
{
    if (sigma <= 0.01 || v.size() < 2) return v;
    int rad = int(std::ceil(sigma * 4));
    std::vector<double> w(2 * rad + 1);
    double sum = 0;
    for (int k = -rad; k <= rad; ++k) sum += w[k + rad] = std::exp(-0.5 * k * k / (sigma * sigma));
    for (double& x : w) x /= sum;
    int n = int(v.size());
    std::vector<Vec3> out(n);
    for (int i = 0; i < n; ++i)
    {
        Vec3 a;
        for (int k = -rad; k <= rad; ++k) a = a + v[mirrorIndex(i + k, n)] * w[k + rad];
        out[i] = a;
    }
    return out;
}

const Mat3 kM = [] { Mat3 m; m.m[0][0] = -1; m.m[2][2] = -1; return m; }();  // GPMF orientation frame -> camera frame

} // namespace

bool loadSource360(const std::wstring& path, Source360Info& info, std::string* err)
{
    Mp4File f;
    if (!f.open(path, err)) return false;
    info = Source360Info();
    info.path = path;
    const Mp4Track* v0 = nullptr;
    const Mp4Track* gpmd = nullptr;
    const Mp4Track* tmcd = nullptr;
    for (const auto& t : f.tracks())
    {
        if (t.handler == "vide" && (t.codec == "hvc1" || t.codec == "hev1" || t.codec == "avc1"))
        {
            if (!v0) v0 = &t;
            ++info.videoTracks;
            info.videoTrackIds.push_back(t.id);
            info.videoFirstSampleSizes.push_back(t.sizes.empty() ? 0 : t.sizes[0]);
        }
        else if (t.codec == "gpmd" && !gpmd) gpmd = &t;
        else if (t.codec == "tmcd" && !tmcd) tmcd = &t;
    }
    if (!v0 || info.videoTracks < 2)
    {
        if (err) *err = "not a GoPro .360 file (needs two video tracks, found " + std::to_string(info.videoTracks) + ")";
        return false;
    }
    info.frames = int(v0->sizes.size());
    info.streamW = v0->width;
    info.streamH = v0->height;
    info.bitDepth = v0->bitDepth;
    if (v0->times.size() >= 2 && v0->timescale)
    {
        info.fpsNum = int(v0->timescale);
        info.fpsDen = int(v0->times[1] - v0->times[0]);
        if (info.fpsDen <= 0) { info.fpsNum = 30000; info.fpsDen = 1001; }
    }
    if (tmcd && !tmcd->sizes.empty())
    {
        std::vector<uint8_t> s;
        if (f.readSample(*tmcd, 0, s) && s.size() >= 4) info.tcStartFrame = be32(s.data());
    }
    if (gpmd)
    {
        GpmfOut g;
        std::vector<uint8_t> s;
        for (size_t i = 0; i < gpmd->sizes.size(); ++i)
            if (f.readSample(*gpmd, i, s)) parseGpmf(s.data(), s.size(), g);
        info.cori = std::move(g.cori);
        info.iori = std::move(g.iori);
        info.grav = std::move(g.grav);
    }
    return true;
}

Stabilizer::Stabilizer(const Source360Info& info) : m_Fps(info.fps())
{
    if (info.cori.empty()) return;
    int n = info.frames;
    m_T.resize(n);
    m_Grav.resize(n);
    for (int i = 0; i < n; ++i)
    {
        const auto& q = info.cori[metaIndex(i, n, info.cori.size())];
        Mat3 Q = Mat3::fromQuat(q[0], q[1], q[2], q[3]);
        Mat3 I;
        if (!info.iori.empty())
        {
            const auto& r = info.iori[metaIndex(i, n, info.iori.size())];
            I = Mat3::fromQuat(r[0], r[1], r[2], r[3]);
        }
        m_T[i] = kM * I * Q * kM.transposed();
        Vec3 g = info.grav.empty() ? Vec3(0, 1, 0) : info.grav[metaIndex(i, n, info.grav.size())];
        m_Grav[i] = (m_T[i].transposed() * (kM * g)).normalized();
    }
}

const std::vector<Mat3>& Stabilizer::table(const StabSettings& s)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Cache.find(s);
    if (it != m_Cache.end()) return it->second;

    int n = int(m_T.size());
    std::vector<Vec3> fwd(n), upc(n);
    for (int i = 0; i < n; ++i)
    {
        Mat3 Tt = m_T[i].transposed();
        fwd[i] = Tt * Vec3(0, 0, 1);
        upc[i] = Tt * Vec3(0, 1, 0);
    }
    std::vector<Vec3> up;
    if (s.horizon) up = gaussSmooth(m_Grav, s.gravitySeconds * m_Fps);
    else if (s.directionLock) up.assign(n, upc[0]);
    else up = gaussSmooth(upc, s.smoothSeconds * m_Fps);
    std::vector<Vec3> f = s.directionLock ? std::vector<Vec3>(n, fwd[0]) : gaussSmooth(fwd, s.smoothSeconds * m_Fps);

    std::vector<Mat3> out(n);
    for (int i = 0; i < n; ++i)
    {
        Vec3 u = up[i].normalized();
        Vec3 z = (f[i] - u * f[i].dot(u));
        if (z.norm() < 1e-6) z = Vec3(0, 0, 1) - u * u.z;  // looking straight up/down: pick any forward
        z = z.normalized();
        Vec3 x = u.cross(z);
        out[i] = Mat3::cols(x, u, z);  // output view -> world0
    }
    return m_Cache.emplace(s, std::move(out)).first->second;
}

Mat3 Stabilizer::rotation(int frame, const StabSettings& s, double subFrame)
{
    if (!s.stabilize || m_T.empty()) return Mat3::identity();
    const auto& e = table(s);
    int n = int(e.size());
    frame = std::clamp(frame, 0, n - 1);
    // The pixels come from `frame`, so the camera orientation is that frame's; only the virtual (output)
    // camera moves within the shutter interval.
    Mat3 view = e[frame];
    if (subFrame != 0)
    {
        double t = std::clamp(frame + subFrame, 0.0, double(n - 1));
        int a = int(std::floor(t));
        int b = std::min(a + 1, n - 1);
        view = Quat::slerp(Quat::fromMat(e[a]), Quat::fromMat(e[b]), t - a).toMat();
    }
    return m_T[frame] * view;
}
