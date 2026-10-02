// Reprojection setup (shared by all render paths) and the CPU renderer.
// The per-pixel math lives in reframe_kernel.h so CPU, CUDA and OpenCL produce identical results.
#pragma once

#include "decoder.h"
#include "mathx.h"
#include "reframe_kernel.h"

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
    Lens = 0,             // rectilinear .. stereographic .. fisheye, controlled by curvature
    Equirectangular = 1,  // full 360x180 sphere
};

struct ViewParams
{
    Mat3 R;                 // output view direction -> camera direction
    Projection proj = Projection::Lens;
    double fovH = 1.745;    // horizontal field of view, radians
    double curvature = 0.0; // 0 = rectilinear, 1 = stereographic, 2 = equidistant fisheye
    int supersample = 1;    // NxN samples per pixel
};

// Kernel parameters for a frame. outStride is in floats per output row.
RfParams makeRfParams(const ViewParams& vp, const Nv12Frame& stream, int outW, int outH, int outStride);

class Reprojector
{
public:
    Reprojector(const Nv12Frame& s0, const Nv12Frame& s1, const ViewParams& vp, int fullW, int fullH);
    // Render pixels [x0, x1) of row yTop (0 = top row of the full frame) into dst (RGBA float, 4 per pixel).
    void renderRow(int yTop, int x0, int x1, float* dst) const;
    // Render the whole frame with all CPU cores. dst rows are bottom-up with rowStride floats per row.
    void renderAll(float* dst, ptrdiff_t rowStride) const;

private:
    const Nv12Frame& m_S0;
    const Nv12Frame& m_S1;
    RfParams m_P;
};
