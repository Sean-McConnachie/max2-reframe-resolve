#include "gopro_view.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kW = 3840, kH = 2160;
constexpr double kCurvStep = 0.005;

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// Pixel distances from the center over the whole frame, as weighted bins.
struct Radii
{
    std::vector<double> r, w;
    Radii()
    {
        const int bins = 320;
        const double rMax = std::hypot(kW / 2, kH / 2);
        r.resize(bins);
        w.assign(bins, 0);
        for (int i = 0; i < bins; ++i) r[i] = (i + 0.5) * rMax / bins;
        const int step = 8;
        for (double y = step / 2.0; y < kH; y += step)
            for (double x = step / 2.0; x < kW; x += step)
            {
                double d = std::hypot(x - kW / 2, y - kH / 2);
                w[std::min(bins - 1, int(d / rMax * bins))] += 1;
            }
        double sum = 0;
        for (double v : w) sum += v;
        for (double& v : w) v /= sum;
    }
};

const Radii& radii()
{
    static const Radii r;
    return r;
}

} // namespace

double goproAngle(double r, double zoom, double lensCurve)
{
    // The plugin's host code scales the shader's frame size by 1/1.1 and passes Lens Curve / 200 (both measured).
    const double m = std::max(kW, kH) / 1.1;
    const double distort = lensCurve / 200.0;
    double radius = r * 100.0 / std::max(zoom, 1e-3);
    double fov = distort * 65.0;
    double fovAbs = std::fabs(fov) + 0.1;
    double fovRad = 0.785398 * fovAbs / 45.0;
    double tfr = std::tan(fovRad);
    double maxr = std::sqrt((m * m + 0.5625 * m * m) / 4.0);
    double maxrV = 0.5 * m * 9.0 / 16.0 * fovAbs / (57.2958 * std::atan(std::tan(0.785398 * fovAbs / 45.0)));
    double a = clamp01(fov * 10000.0);
    double val = std::clamp(radius / maxr * fovRad, 0.0, 1.57);
    radius = (1 - a) * (maxr * std::tan(val) / tfr) + a * (maxrV * 57.2958 * std::atan(radius / maxrV * tfr) / fovAbs);
    double rn = radius / maxr;
    double a2 = clamp01((rn - 0.9) * 10000.0), aa = clamp01((rn - 1.1) * 10000.0);
    double phi = (1 - a2) * (rn * 85.6 - rn * rn * 10.91) + a2 * (-rn * rn * rn * 13.9167 + rn * rn * 12.35 + rn * 76.2542 - 0.284);
    phi = (1 - aa) * phi + aa * (2.0 * 57.2957795131 * std::atan(rn * 0.7630377885));
    return std::min(phi, 180.0);
}

double max2Angle(double r, double fov, double curv)
{
    // as makeRfParams() and rf_view_dir() in the plugin
    double k = std::clamp(1.0 - std::clamp(curv, 0.0, 2.0) * 0.5, 0.0, 1.0);
    double tm = std::min(fov * kPi / 360.0, kPi);
    if (k > 1e-4) tm = std::min(tm, kPi / (2 * k) - 0.01);
    tm = std::max(tm, 0.001);
    double rs = k > 1e-4 ? std::tan(k * tm) / k : tm;
    double rho = r / (kW / 2) * rs;
    double th = k > 1e-4 ? std::atan(k * rho) / k : rho;
    return std::min(th * 180.0 / kPi, 180.0);
}

double max2Radius(double angle, double fov, double curv)
{
    double k = std::clamp(1.0 - std::clamp(curv, 0.0, 2.0) * 0.5, 0.0, 1.0);
    double tm = std::min(fov * kPi / 360.0, kPi);
    if (k > 1e-4) tm = std::min(tm, kPi / (2 * k) - 0.01);
    tm = std::max(tm, 0.001);
    double rs = k > 1e-4 ? std::tan(k * tm) / k : tm;
    double th = angle * kPi / 180.0;
    if (k > 1e-4 && k * th >= kPi / 2 - 1e-6) return 1e9;  // the lens cannot show this direction
    double rho = k > 1e-4 ? std::tan(k * th) / k : th;
    return rho / rs * (kW / 2);
}

Max2Lens fitLens(double zoom, double lensCurve, const Max2Lens* start, double fixedCurv)
{
    const Radii& R = radii();
    std::vector<double> target(R.r.size());
    for (size_t i = 0; i < R.r.size(); ++i) target[i] = goproAngle(R.r[i], zoom, lensCurve);
    // For each distance from the center: how far from it Max2 shows the direction that GoPro shows there. This
    // is what the eye compares, and it weighs the stretched edges of wide views correctly.
    auto offsets = [&](double fov, double curv, std::vector<double>& d) {
        d.resize(R.r.size());
        for (size_t i = 0; i < R.r.size(); ++i) d[i] = std::min(std::fabs(max2Radius(target[i], fov, curv) - R.r[i]), 1e4);
    };
    std::vector<double> d;
    auto cost = [&](double fov, double curv) {
        offsets(fov, curv, d);
        double s = 0;
        for (size_t i = 0; i < R.r.size(); ++i) s += R.w[i] * d[i] * d[i];
        return s;
    };
    const bool fixed = fixedCurv >= 0;
    double bestFov = 100, bestCurv = fixed ? std::min(fixedCurv, 2.0) : 0, best = 1e300;
    if (start)
    {
        bestFov = start->fov;
        if (!fixed) bestCurv = start->curv;
        best = cost(bestFov, bestCurv);
    }
    else
        for (double curv = fixed ? bestCurv : 0; curv <= 2.0001; curv += fixed ? 10 : 0.05)
            for (double fov = 10; fov <= 360; fov += 2)
            {
                double c = cost(fov, curv);
                if (c < best) best = c, bestFov = fov, bestCurv = curv;
            }
    if (!fixed)
    {
        double sf = 1, sc = 0.025;
        for (int it = 0; it < 400 && (sf > 1e-4 || sc > 1e-6); ++it)
        {
            bool moved = false;
            const double cand[4][2] = {{bestFov + sf, bestCurv}, {bestFov - sf, bestCurv}, {bestFov, bestCurv + sc}, {bestFov, bestCurv - sc}};
            for (const auto& c : cand)
            {
                double f = std::clamp(c[0], 1.0, 360.0), cv = std::clamp(c[1], 0.0, 2.0);
                double v = cost(f, cv);
                if (v < best) best = v, bestFov = f, bestCurv = cv, moved = true;
            }
            if (!moved) sf *= 0.5, sc *= 0.5;
        }
        // The cost changes very little with the curvature near its best value, so the exact optimum wanders from
        // one setting to the next. A rounded curvature (with the field of view fitted again) changes the picture
        // by a fraction of a pixel and keeps animated clips free of needless curvature keyframes.
        bestCurv = std::clamp(std::round(bestCurv / kCurvStep) * kCurvStep, 0.0, 2.0);
        best = cost(bestFov, bestCurv);
    }
    for (double sf = 1; sf > 1e-4;)
    {
        double up = std::min(bestFov + sf, 360.0), down = std::max(bestFov - sf, 1.0);
        double cu = cost(up, bestCurv), cd = cost(down, bestCurv);
        if (cu < best) best = cu, bestFov = up;
        else if (cd < best) best = cd, bestFov = down;
        else sf *= 0.5;
    }
    Max2Lens out;
    out.fov = bestFov;
    out.curv = bestCurv;
    offsets(bestFov, bestCurv, d);
    for (size_t i = 0; i < R.r.size(); ++i)
    {
        out.meanErr += R.w[i] * d[i];
        if (R.w[i] > 0) out.maxErr = std::max(out.maxErr, d[i]);
    }
    return out;
}
