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
    int extra = std::max(0, w - 3 * h);  // 128 on Max 2 (5888 = 3 * 1920 + 128)
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
    return p;
}

Reprojector::Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, const ViewParams& vp, int fullW, int fullH)
    : m_S0(s0), m_S1(s1), m_P(makeRfParams(vp, s0, fullW, fullH, fullW * 4))
{
}

void Reprojector::renderRow(int yTop, int x0, int x1, float* dst) const
{
    for (int x = x0; x < x1; ++x, dst += 4) rf_shade(&m_P, m_S0.y(), m_S0.uv(), m_S1.y(), m_S1.uv(), x, yTop, dst);
}

void Reprojector::renderAll(float* dst, ptrdiff_t rowStride) const
{
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next{0};
    auto work = [&] {
        for (int r; (r = next.fetch_add(4)) < m_P.outH;)
            for (int row = r; row < std::min(r + 4, m_P.outH); ++row)
                renderRow(m_P.outH - 1 - row, 0, m_P.outW, dst + row * rowStride);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}
