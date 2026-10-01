// CPU reprojection: GoPro dual-stream EAC (NV12) -> flat view or equirectangular, float RGBA output.
#pragma once

#include "decoder.h"
#include "mathx.h"

// GoPro EAC layout inside one stream: [face A | middle face | face C]. Faces A and C are split by the lens
// seam into two halves that overlap by `ovl` face columns.
struct EacLayout
{
    int face = 0;   // face size in pixels (stream height)
    int half = 0;   // width of each half of a split face
    int ovl = 0;    // overlap of the two halves in face columns
    int mid = 0;    // x of the middle face
    int right = 0;  // x of the right-slot face
    static EacLayout fromStream(int w, int h);
};

enum class Projection
{
    Lens = 0,             // rectilinear .. stereographic, controlled by curvature
    Equirectangular = 1,  // full 360x180 sphere
};

struct ViewParams
{
    Mat3 R;                 // output view direction -> camera direction
    Projection proj = Projection::Lens;
    double fovH = 1.745;    // horizontal field of view, radians
    double curvature = 0.0; // 0 = rectilinear, 1 = stereographic
    int supersample = 1;    // NxN samples per pixel
};

class Reprojector
{
public:
    Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, const ViewParams& vp, int fullW, int fullH);
    // Render pixels [x0, x1) of row yTop (0 = top row of the full frame) into dst (RGBA float, 4 per pixel).
    void renderRow(int yTop, int x0, int x1, float* dst) const;

private:
    void direction(double px, double py, float d[3]) const;
    void sample(const float d[3], float rgb[3]) const;

    const Nv12Frame& m_S0;
    const Nv12Frame& m_S1;
    EacLayout m_L;
    ViewParams m_Vp;
    float m_R[3][3];
    int m_W, m_H;
    double m_Gmax = 1;
};
