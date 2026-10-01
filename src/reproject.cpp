#include "reproject.h"

#include <algorithm>
#include <cmath>

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

namespace {

// general perspective: radius on the image plane for a ray at angle theta from the view axis
double lensG(double theta, double d) { return (d + 1) * std::sin(theta) / (d + std::cos(theta)); }

struct Yuv { float y, u, v; };

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Bilinear NV12 sample at stream pixel (x, y); x is clamped to [xmin, xmax].
inline Yuv sampleNv12(const Nv12Frame& f, float x, float y, float xmin, float xmax)
{
    x = std::clamp(x, xmin, xmax);
    y = std::clamp(y, 0.0f, float(f.height - 1));
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, int(xmax)), y1 = std::min(y0 + 1, f.height - 1);
    float fx = x - x0, fy = y - y0;
    const uint8_t* Y = f.y();
    size_t p = size_t(f.pitch);
    float top = lerp(Y[y0 * p + x0], Y[y0 * p + x1], fx);
    float bot = lerp(Y[y1 * p + x0], Y[y1 * p + x1], fx);
    Yuv r;
    r.y = lerp(top, bot, fy);

    // chroma: half resolution, interleaved UV
    float cx = std::clamp(x * 0.5f - 0.25f, xmin * 0.5f, xmax * 0.5f);
    float cy = std::clamp(y * 0.5f - 0.25f, 0.0f, float(f.height / 2 - 1));
    int cx0 = int(cx), cy0 = int(cy);
    int cx1 = std::min(cx0 + 1, int(xmax * 0.5f)), cy1 = std::min(cy0 + 1, f.height / 2 - 1);
    float gx = cx - cx0, gy = cy - cy0;
    const uint8_t* UV = f.uv();
    auto at = [&](int xx, int yy, int c) { return float(UV[yy * p + xx * 2 + c]); };
    r.u = lerp(lerp(at(cx0, cy0, 0), at(cx1, cy0, 0), gx), lerp(at(cx0, cy1, 0), at(cx1, cy1, 0), gx), gy);
    r.v = lerp(lerp(at(cx0, cy0, 1), at(cx1, cy0, 1), gx), lerp(at(cx0, cy1, 1), at(cx1, cy1, 1), gx), gy);
    return r;
}

constexpr float kFourOverPi = 1.27323954f;

// atan for |x| <= 1, max error ~1e-5 rad (0.01 px on a 1920 px face)
inline float atanUnit(float x)
{
    float x2 = x * x;
    return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
}

} // namespace

Reprojector::Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, const ViewParams& vp, int fullW, int fullH)
    : m_S0(s0), m_S1(s1), m_L(EacLayout::fromStream(s0.width, s0.height)), m_Vp(vp), m_W(fullW), m_H(fullH)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m_R[i][j] = float(vp.R.m[i][j]);
    double d = std::clamp(vp.curvature, 0.0, 1.0);
    m_Vp.curvature = d;
    double thetaMax = std::min(vp.fovH * 0.5, std::acos(-d) - 0.02);
    thetaMax = std::max(thetaMax, 0.01);
    m_Gmax = lensG(thetaMax, d);
}

void Reprojector::direction(double px, double py, float out[3]) const
{
    double v[3];
    if (m_Vp.proj == Projection::Equirectangular)
    {
        double lon = px / m_W * 2 * kPi - kPi;
        double lat = kPi / 2 - py / m_H * kPi;
        v[0] = std::cos(lat) * std::sin(lon);
        v[1] = std::sin(lat);
        v[2] = std::cos(lat) * std::cos(lon);
    }
    else
    {
        double half = m_W * 0.5;
        double nx = (px - half) / half;
        double ny = (m_H * 0.5 - py) / half;
        double r = std::sqrt(nx * nx + ny * ny);
        if (r < 1e-12)
        {
            v[0] = 0; v[1] = 0; v[2] = 1;
        }
        else
        {
            double d = m_Vp.curvature;
            double rho = r * m_Gmax / (d + 1);
            double rho2 = rho * rho;
            double c = (-rho2 * d + std::sqrt(1 + rho2 * (1 - d * d))) / (1 + rho2);
            double s = std::sqrt(std::max(0.0, 1 - c * c));
            v[0] = s * nx / r;
            v[1] = s * ny / r;
            v[2] = c;
        }
    }
    for (int i = 0; i < 3; ++i) out[i] = float(m_R[i][0] * v[0] + m_R[i][1] * v[1] + m_R[i][2] * v[2]);
}

void Reprojector::sample(const float d[3], float rgb[3]) const
{
    float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    const Nv12Frame* f;
    int slot;
    float u, v;
    // face selection; see proto/eac.py FACES for the derivation
    if (ax >= ay && ax >= az)
    {
        f = &m_S0;
        if (d[0] > 0) { slot = 2; u = -d[2] / ax; v = -d[1] / ax; }  // right
        else          { slot = 0; u = d[2] / ax;  v = -d[1] / ax; }  // left
    }
    else if (az >= ay)
    {
        if (d[2] > 0) { f = &m_S0; slot = 1; u = d[0] / az; v = -d[1] / az; }  // front
        else          { f = &m_S1; slot = 1; u = d[1] / az; v = -d[0] / az; }  // back
    }
    else
    {
        f = &m_S1;
        if (d[1] > 0) { slot = 2; u = d[2] / ay;  v = -d[0] / ay; }  // top
        else          { slot = 0; u = -d[2] / ay; v = -d[0] / ay; }  // bottom
    }
    const float face = float(m_L.face);
    float cu = (atanUnit(u) * kFourOverPi + 1) * 0.5f * face - 0.5f;
    float cv = (atanUnit(v) * kFourOverPi + 1) * 0.5f * face - 0.5f;

    Yuv s;
    if (slot == 1)
    {
        s = sampleNv12(*f, m_L.mid + cu, cv, 0.0f, float(f->width - 1));
    }
    else
    {
        float base = slot == 0 ? 0.0f : float(m_L.right);
        float half = float(m_L.half);
        float startB = face - half;  // first face column held by the second half
        float wb = std::clamp((cu - startB) / float(std::max(1, m_L.ovl)), 0.0f, 1.0f);
        Yuv a{}, b{};
        if (wb < 1) a = sampleNv12(*f, base + cu, cv, base, base + half - 1);
        if (wb > 0) b = sampleNv12(*f, base + half + (cu - startB), cv, base + half, base + 2 * half - 1);
        s.y = a.y * (1 - wb) + b.y * wb;
        s.u = a.u * (1 - wb) + b.u * wb;
        s.v = a.v * (1 - wb) + b.v * wb;
    }
    // full-range BT.709
    float Y = s.y * (1.0f / 255), Cb = (s.u - 128) * (1.0f / 255), Cr = (s.v - 128) * (1.0f / 255);
    rgb[0] = Y + 1.5748f * Cr;
    rgb[1] = Y - 0.187324f * Cb - 0.468124f * Cr;
    rgb[2] = Y + 1.8556f * Cb;
}

void Reprojector::renderRow(int yTop, int x0, int x1, float* dst) const
{
    const int ss = std::max(1, m_Vp.supersample);
    const float inv = 1.0f / float(ss * ss);
    for (int x = x0; x < x1; ++x, dst += 4)
    {
        float acc[3] = {0, 0, 0};
        for (int sy = 0; sy < ss; ++sy)
            for (int sx = 0; sx < ss; ++sx)
            {
                float d[3], rgb[3];
                direction(x + (sx + 0.5) / ss, yTop + (sy + 0.5) / ss, d);
                sample(d, rgb);
                acc[0] += rgb[0]; acc[1] += rgb[1]; acc[2] += rgb[2];
            }
        dst[0] = acc[0] * inv;
        dst[1] = acc[1] * inv;
        dst[2] = acc[2] * inv;
        dst[3] = 1.0f;
    }
}
