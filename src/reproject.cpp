#include "reproject.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

EacLayout EacLayout::fromStream(int w, int h)
{
    EacLayout l;
    l.face = h;
    int extra = std::max(0, w - 3 * h);  // Max 2: 128 in 8-bit files (5888 wide), 192 in 10-bit files (5952 wide)
    l.ovl = extra / 2;
    l.half = h / 2 + extra / 4;
    l.mid = 2 * l.half;
    l.right = l.mid + h;
    return l;
}

RfParams makeRfParams(const ViewParams& vp, const Nv12Frame& stream, int outW, int outH, int outStride)
{
    RfParams p = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) p.R[i * 3 + j] = float(vp.R.m[i][j]);
    p.proj = vp.proj == Projection::Equirectangular ? 1 : 0;

    // lens family r = tan(k*theta)/k: k = 1 rectilinear, 0.5 stereographic, 0 equidistant fisheye
    double k = std::clamp(1.0 - std::clamp(vp.curvature, 0.0, 2.0) * 0.5, 0.0, 1.0);
    double thetaMax = std::min(vp.fovH * 0.5, kPi);
    if (k > 1e-4) thetaMax = std::min(thetaMax, kPi / (2 * k) - 0.01);
    thetaMax = std::max(thetaMax, 0.001);
    p.k = float(k);
    p.rhoScale = float(k > 1e-4 ? std::tan(k * thetaMax) / k : thetaMax);
    // FOV only sets the scale; corners beyond it still show the sphere, up to straight behind the camera
    p.thetaMax = float(kPi);

    p.outW = outW;
    p.outH = outH;
    p.outStride = outStride;
    p.ss = std::max(1, vp.supersample);
    EacLayout l = EacLayout::fromStream(stream.width, stream.height);
    p.face = l.face;
    p.halfW = l.half;
    p.ovl = l.ovl;
    p.mid = l.mid;
    p.right = l.right;
    p.srcW = stream.width;
    p.srcH = stream.height;
    p.srcPitch = stream.pitch;
    p.uvOffset = stream.pitch * stream.height;
    // Full-range video. P010 keeps the 10-bit code in the high bits: white is 1023 << 6, zero chroma is 512 << 6.
    p.bps = stream.bytesPerSample;
    p.scale = p.bps == 2 ? 1.0f / 65472.0f : 1.0f / 255.0f;
    p.chromaZero = p.bps == 2 ? 32768.0f : 128.0f;
    return p;
}

Reprojector::Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, std::vector<RfParams> steps)
    : m_S0(s0), m_S1(s1), m_Steps(std::move(steps))
{
}

Reprojector::Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, const ViewParams& vp, int fullW, int fullH)
    : Reprojector(s0, s1, std::vector<RfParams>{makeRfParams(vp, s0, fullW, fullH, fullW * 4)})
{
}

void Reprojector::renderRow(int yTop, int x0, int x1, float* dst) const
{
    const float inv = 1.0f / float(m_Steps.size());
    for (int x = x0; x < x1; ++x, dst += 4)
    {
        float acc[3] = {0, 0, 0};
        for (const RfParams& p : m_Steps)
        {
            float px[4];
            rf_shade(&p, m_S0.y(), m_S0.uv(), m_S1.y(), m_S1.uv(), x, yTop, px);
            acc[0] += px[0]; acc[1] += px[1]; acc[2] += px[2];
        }
        dst[0] = acc[0] * inv;
        dst[1] = acc[1] * inv;
        dst[2] = acc[2] * inv;
        dst[3] = 1.0f;
    }
}

void Reprojector::renderAll(float* dst, ptrdiff_t rowStride) const
{
    const int W = m_Steps[0].outW, H = m_Steps[0].outH;
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next{0};
    auto work = [&] {
        for (int r; (r = next.fetch_add(4)) < H;)
            for (int row = r; row < std::min(r + 4, H); ++row) renderRow(H - 1 - row, 0, W, dst + row * rowStride);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}
