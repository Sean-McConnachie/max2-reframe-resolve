/* Per-pixel reframe math shared by the CPU path (C++), CUDA and OpenCL C.
 * Keep this file plain C: it is also embedded as OpenCL source at build time.
 *
 * Camera coordinates: +x right, +y up, +z forward (front lens).
 * Input: two NV12 frames in GoPro EAC layout (see EacLayout in reproject.h).
 * Output: RGBA float, row 0 of the buffer is the BOTTOM row of the image (OFX convention).
 */
#ifndef REFRAME_KERNEL_H
#define REFRAME_KERNEL_H

#if defined(__CUDACC__)
#define RF_FN __device__ __forceinline__
#define RF_GLOBAL
typedef unsigned char rf_uchar;
#define RF_SQRT sqrtf
#define RF_SIN sinf
#define RF_COS cosf
#define RF_ATAN atanf
#define RF_TAN tanf
#define RF_FABS fabsf
#define RF_FLOOR floorf
#elif defined(__OPENCL_VERSION__)
#define RF_FN
#define RF_GLOBAL __global
typedef uchar rf_uchar;
#define RF_SQRT sqrt
#define RF_SIN sin
#define RF_COS cos
#define RF_ATAN atan
#define RF_TAN tan
#define RF_FABS fabs
#define RF_FLOOR floor
#else
#include <math.h>
#define RF_FN static inline
#define RF_GLOBAL
typedef unsigned char rf_uchar;
#define RF_SQRT sqrtf
#define RF_SIN sinf
#define RF_COS cosf
#define RF_ATAN atanf
#define RF_TAN tanf
#define RF_FABS fabsf
#define RF_FLOOR floorf
#endif

/* All fields are 4 bytes so the layout is identical in C++, CUDA and OpenCL. */
typedef struct RfParams
{
    float R[9];       /* row-major: output view direction -> camera direction */
    int proj;         /* 0 = lens, 1 = equirectangular */
    float k;          /* lens family: radius = tan(k*theta)/k; 1 rectilinear, 0.5 stereographic, 0 equidistant */
    float rhoScale;   /* lens radius at the left/right frame edge */
    float thetaMax;   /* rays beyond this angle from the view axis are black */
    int outW, outH;   /* full output frame */
    int outStride;    /* floats per output row */
    int ss;           /* supersampling (ss x ss per pixel) */
    int face, halfW, ovl, mid, right; /* EAC layout */
    int srcW, srcH, srcPitch;        /* NV12 stream size and row pitch (bytes) */
    int uvOffset;                    /* bytes from the Y plane to the interleaved UV plane */
} RfParams;

#define RF_FOUR_OVER_PI 1.27323954f
#define RF_PI 3.14159265f

/* atan for |x| <= 1, max error ~1e-5 rad */
RF_FN float rf_atan_unit(float x)
{
    float x2 = x * x;
    return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
}

RF_FN float rf_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
RF_FN int rf_mini(int a, int b) { return a < b ? a : b; }

/* Bilinear NV12 sample at stream pixel (x, y), x clamped to [xmin, xmax]. Returns Y, U, V in 0..255. */
RF_FN void rf_sample_nv12(RF_GLOBAL const rf_uchar* Y, RF_GLOBAL const rf_uchar* UV, int pitch, int h,
                          float x, float y, float xmin, float xmax, float* oy, float* ou, float* ov)
{
    x = rf_clampf(x, xmin, xmax);
    y = rf_clampf(y, 0.0f, (float)(h - 1));
    int x0 = (int)x, y0 = (int)y;
    int x1 = rf_mini(x0 + 1, (int)xmax), y1 = rf_mini(y0 + 1, h - 1);
    float fx = x - (float)x0, fy = y - (float)y0;
    float a = Y[y0 * pitch + x0], b = Y[y0 * pitch + x1], c = Y[y1 * pitch + x0], d = Y[y1 * pitch + x1];
    *oy = (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;

    float cx = rf_clampf(x * 0.5f - 0.25f, xmin * 0.5f, xmax * 0.5f);
    float cy = rf_clampf(y * 0.5f - 0.25f, 0.0f, (float)(h / 2 - 1));
    int cx0 = (int)cx, cy0 = (int)cy;
    int cx1 = rf_mini(cx0 + 1, (int)(xmax * 0.5f)), cy1 = rf_mini(cy0 + 1, h / 2 - 1);
    float gx = cx - (float)cx0, gy = cy - (float)cy0;
    int i00 = cy0 * pitch + cx0 * 2, i10 = cy0 * pitch + cx1 * 2, i01 = cy1 * pitch + cx0 * 2, i11 = cy1 * pitch + cx1 * 2;
    float u0 = UV[i00] + (UV[i10] - (float)UV[i00]) * gx, u1 = UV[i01] + (UV[i11] - (float)UV[i01]) * gx;
    float v0 = UV[i00 + 1] + (UV[i10 + 1] - (float)UV[i00 + 1]) * gx, v1 = UV[i01 + 1] + (UV[i11 + 1] - (float)UV[i01 + 1]) * gx;
    *ou = u0 + (u1 - u0) * gy;
    *ov = v0 + (v1 - v0) * gy;
}

/* View direction (output space) for output pixel position (px, py), py measured from the top.
 * Returns 0 if the pixel is outside the lens' coverage. */
RF_FN int rf_view_dir(const RfParams* p, float px, float py, float* v)
{
    if (p->proj == 1)
    {
        float lon = px / (float)p->outW * 2.0f * RF_PI - RF_PI;
        float lat = RF_PI * 0.5f - py / (float)p->outH * RF_PI;
        float cl = RF_COS(lat);
        v[0] = cl * RF_SIN(lon);
        v[1] = RF_SIN(lat);
        v[2] = cl * RF_COS(lon);
        return 1;
    }
    float hw = (float)p->outW * 0.5f;
    float nx = (px - hw) / hw;
    float ny = ((float)p->outH * 0.5f - py) / hw;
    float r = RF_SQRT(nx * nx + ny * ny);
    if (r < 1e-9f)
    {
        v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f;
        return 1;
    }
    float rho = r * p->rhoScale;
    float theta = p->k > 1e-4f ? RF_ATAN(p->k * rho) / p->k : rho;
    if (theta > p->thetaMax + 1e-4f) return 0;
    float s = RF_SIN(theta);
    v[0] = s * nx / r;
    v[1] = s * ny / r;
    v[2] = RF_COS(theta);
    return 1;
}

/* Camera direction -> RGB (BT.709 full range). */
RF_FN void rf_sample_dir(const RfParams* p, RF_GLOBAL const rf_uchar* Y0, RF_GLOBAL const rf_uchar* UV0,
                         RF_GLOBAL const rf_uchar* Y1, RF_GLOBAL const rf_uchar* UV1, const float* d, float* rgb)
{
    float ax = RF_FABS(d[0]), ay = RF_FABS(d[1]), az = RF_FABS(d[2]);
    int second, slot;
    float u, v;
    /* face selection; see proto/eac.py FACES for the derivation */
    if (ax >= ay && ax >= az)
    {
        second = 0;
        if (d[0] > 0.0f) { slot = 2; u = -d[2] / ax; v = -d[1] / ax; } /* right */
        else             { slot = 0; u = d[2] / ax;  v = -d[1] / ax; } /* left */
    }
    else if (az >= ay)
    {
        if (d[2] > 0.0f) { second = 0; slot = 1; u = d[0] / az; v = -d[1] / az; } /* front */
        else             { second = 1; slot = 1; u = d[1] / az; v = -d[0] / az; } /* back */
    }
    else
    {
        second = 1;
        if (d[1] > 0.0f) { slot = 2; u = d[2] / ay;  v = -d[0] / ay; } /* top */
        else             { slot = 0; u = -d[2] / ay; v = -d[0] / ay; } /* bottom */
    }
    RF_GLOBAL const rf_uchar* Y = second ? Y1 : Y0;
    RF_GLOBAL const rf_uchar* UV = second ? UV1 : UV0;
    float face = (float)p->face;
    float cu = (rf_atan_unit(u) * RF_FOUR_OVER_PI + 1.0f) * 0.5f * face - 0.5f;
    float cv = (rf_atan_unit(v) * RF_FOUR_OVER_PI + 1.0f) * 0.5f * face - 0.5f;

    float sy, su, sv;
    if (slot == 1)
    {
        rf_sample_nv12(Y, UV, p->srcPitch, p->srcH, (float)p->mid + cu, cv, 0.0f, (float)(p->srcW - 1), &sy, &su, &sv);
    }
    else
    {
        /* split face: two halves overlapping by ovl face columns, blended across the overlap */
        float base = slot == 0 ? 0.0f : (float)p->right;
        float hf = (float)p->halfW;
        float startB = face - hf;
        float wb = rf_clampf((cu - startB) / (float)(p->ovl > 0 ? p->ovl : 1), 0.0f, 1.0f);
        float ay_ = 0, au = 0, av = 0, by = 0, bu = 0, bv = 0;
        if (wb < 1.0f) rf_sample_nv12(Y, UV, p->srcPitch, p->srcH, base + cu, cv, base, base + hf - 1.0f, &ay_, &au, &av);
        if (wb > 0.0f) rf_sample_nv12(Y, UV, p->srcPitch, p->srcH, base + hf + (cu - startB), cv, base + hf, base + 2.0f * hf - 1.0f, &by, &bu, &bv);
        sy = ay_ + (by - ay_) * wb;
        su = au + (bu - au) * wb;
        sv = av + (bv - av) * wb;
    }
    float yy = sy * (1.0f / 255.0f), cb = (su - 128.0f) * (1.0f / 255.0f), cr = (sv - 128.0f) * (1.0f / 255.0f);
    rgb[0] = yy + 1.5748f * cr;
    rgb[1] = yy - 0.187324f * cb - 0.468124f * cr;
    rgb[2] = yy + 1.8556f * cb;
}

/* Shade output pixel (x, yTop) with supersampling. */
RF_FN void rf_shade(const RfParams* p, RF_GLOBAL const rf_uchar* Y0, RF_GLOBAL const rf_uchar* UV0,
                    RF_GLOBAL const rf_uchar* Y1, RF_GLOBAL const rf_uchar* UV1, int x, int yTop, float* out)
{
    int ss = p->ss > 0 ? p->ss : 1;
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f;
    for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx)
        {
            float v[3], d[3], rgb[3];
            float px = (float)x + ((float)sx + 0.5f) / (float)ss;
            float py = (float)yTop + ((float)sy + 0.5f) / (float)ss;
            if (!rf_view_dir(p, px, py, v)) continue;
            d[0] = p->R[0] * v[0] + p->R[1] * v[1] + p->R[2] * v[2];
            d[1] = p->R[3] * v[0] + p->R[4] * v[1] + p->R[5] * v[2];
            d[2] = p->R[6] * v[0] + p->R[7] * v[1] + p->R[8] * v[2];
            rf_sample_dir(p, Y0, UV0, Y1, UV1, d, rgb);
            acc0 += rgb[0]; acc1 += rgb[1]; acc2 += rgb[2];
        }
    float inv = 1.0f / (float)(ss * ss);
    out[0] = acc0 * inv;
    out[1] = acc1 * inv;
    out[2] = acc2 * inv;
    out[3] = 1.0f;
}

#if defined(__OPENCL_VERSION__)
__kernel void ReframeKernel(RfParams p, __global const uchar* Y0, __global const uchar* Y1, __global float* out)
{
    int x = get_global_id(0);
    int y = get_global_id(1);
    if (x >= p.outW || y >= p.outH) return;
    float px[4];
    rf_shade(&p, Y0, Y0 + p.uvOffset, Y1, Y1 + p.uvOffset, x, p.outH - 1 - y, px);
    __global float* o = out + y * p.outStride + x * 4;
    o[0] = px[0]; o[1] = px[1]; o[2] = px[2]; o[3] = px[3];
}
#endif

#endif /* REFRAME_KERNEL_H */
