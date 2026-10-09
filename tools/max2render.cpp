// Command-line test harness for the reframe engine (no Resolve needed).
//   max2render file.360 [--frame N] [--count N] [--out out.ppm] [--w 1920] [--h 1080] [--proj lens|erp]
//              [--fov 100] [--curv 0.4] [--pan 0] [--tilt 0] [--roll 0] [--stab 1] [--horizon 1] [--dirlock 0]
//              [--smooth 0.3] [--ss 1] [--gpu cpu|cuda|opencl] [--repeat N] [--dump prefix]
//              [--bits 8|16] (bits of the PPM)
//              [--mb shutterAngle] [--mbmax 32] [--panrate deg/frame] [--tiltrate deg/frame] [--fovrate deg/frame]
//              [--also other.360] (open one more clip first and decode its frame 0; can be repeated)
//              [--idle seconds] (wait after the last frame, then decode it again and report the time)
//              [--thumb 1] (as the plugin does for thumbnails: nearest earlier keyframe, no read-ahead)
// With --count > 1 it renders a sequence and reports timing; %d in --out is replaced by the frame number.
#include <windows.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/engine.h"
#include "../src/gpu.h"

// rgba rows are bottom-up (OFX convention)
static void writePpm(const std::string& path, const std::vector<float>& rgba, int w, int h, int bits)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const int bytes = bits > 8 ? 2 : 1;
    const float maxv = bytes == 2 ? 65535.0f : 255.0f;
    fprintf(f, "P6\n%d %d\n%d\n", w, h, int(maxv));
    std::vector<uint8_t> row(size_t(w) * 3 * bytes);
    for (int y = h - 1; y >= 0; --y)
    {
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                unsigned v = unsigned(std::clamp(rgba[(size_t(y) * w + x) * 4 + c], 0.0f, 1.0f) * maxv + 0.5f);
                uint8_t* o = row.data() + (size_t(x) * 3 + c) * bytes;
                if (bytes == 2) { o[0] = uint8_t(v >> 8); o[1] = uint8_t(v); }  // PPM is big-endian
                else o[0] = uint8_t(v);
            }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

static void dumpStreams(const std::string& prefix, const Nv12Frame& a, const Nv12Frame& b)
{
    for (int k = 0; k < 2; ++k)
    {
        const Nv12Frame& fr = k ? b : a;
        FILE* df = fopen((prefix + std::to_string(k) + ".pgm").c_str(), "wb");
        if (!df) continue;
        fprintf(df, "P5\n%d %d\n%d\n", fr.width, fr.height, fr.bytesPerSample == 2 ? 65535 : 255);
        std::vector<uint8_t> row(fr.y(), fr.y() + fr.pitch);
        for (int y = 0; y < fr.height; ++y)
        {
            std::memcpy(row.data(), fr.y() + size_t(y) * fr.pitch, row.size());
            if (fr.bytesPerSample == 2)
                for (size_t i = 0; i + 1 < row.size(); i += 2) std::swap(row[i], row[i + 1]);  // PGM is big-endian
            fwrite(row.data(), 1, row.size(), df);
        }
        fclose(df);
    }
}

struct GpuTarget
{
    std::string kind = "cpu";
    // CUDA
    cudaStream_t stream = nullptr;
    float* dOut = nullptr;
    // OpenCL
    cl_context ctx = nullptr;
    cl_command_queue queue = nullptr;
    cl_mem clOut = nullptr;

    bool init(size_t bytes)
    {
        if (kind == "cuda")
        {
            return cudaStreamCreate(&stream) == cudaSuccess && cudaMalloc(reinterpret_cast<void**>(&dOut), bytes) == cudaSuccess;
        }
        if (kind == "opencl")
        {
            cl_uint n = 0;
            clGetPlatformIDs(0, nullptr, &n);
            std::vector<cl_platform_id> plats(n);
            clGetPlatformIDs(n, plats.data(), nullptr);
            cl_device_id dev = nullptr;
            for (auto pl : plats)
            {
                char name[256] = {};
                clGetPlatformInfo(pl, CL_PLATFORM_NAME, sizeof(name), name, nullptr);
                cl_device_id d;
                if (clGetDeviceIDs(pl, CL_DEVICE_TYPE_GPU, 1, &d, nullptr) == CL_SUCCESS && (!dev || strstr(name, "NVIDIA")))
                {
                    dev = d;
                    printf("OpenCL platform: %s\n", name);
                }
            }
            if (!dev) return false;
            cl_int e;
            ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &e);
            queue = clCreateCommandQueue(ctx, dev, 0, &e);
            clOut = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &e);
            return e == CL_SUCCESS;
        }
        return true;
    }
};

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: max2render file.360 [options]\n");
        return 2;
    }
    std::string path = argv[1];
    int frame = 0, count = 1, w = 1920, h = 1080, repeat = 1, bits = 8;
    std::string out, dump;
    RenderSettings rs;
    GpuTarget gpu;
    MotionBlurSettings mb;
    double panRate = 0, tiltRate = 0, fovRate = 0;
    std::vector<std::string> also;
    int idle = 0;
    bool thumb = false;
    for (int i = 2; i + 1 < argc; i += 2)
    {
        std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--frame") frame = atoi(v);
        else if (k == "--count") count = atoi(v);
        else if (k == "--out") out = v;
        else if (k == "--w") w = atoi(v);
        else if (k == "--h") h = atoi(v);
        else if (k == "--proj") rs.proj = std::string(v) == "erp" ? Projection::Equirectangular : Projection::Lens;
        else if (k == "--fov") rs.fovDeg = atof(v);
        else if (k == "--curv") rs.curvature = atof(v);
        else if (k == "--pan") rs.pan = atof(v);
        else if (k == "--tilt") rs.tilt = atof(v);
        else if (k == "--roll") rs.roll = atof(v);
        else if (k == "--stab") rs.stab.stabilize = atoi(v) != 0;
        else if (k == "--horizon") rs.stab.horizon = atoi(v) != 0;
        else if (k == "--dirlock") rs.stab.directionLock = atoi(v) != 0;
        else if (k == "--smooth") rs.stab.smoothSeconds = atof(v);
        else if (k == "--ss") rs.supersample = atoi(v);
        else if (k == "--gpu") gpu.kind = v;
        else if (k == "--repeat") repeat = std::max(1, atoi(v));
        else if (k == "--dump") dump = v;
        else if (k == "--bits") bits = atoi(v);
        else if (k == "--mb") { mb.enabled = true; mb.shutterAngle = atof(v); }
        else if (k == "--mbmax") mb.maxSamples = atoi(v);
        else if (k == "--panrate") panRate = atof(v);
        else if (k == "--tiltrate") tiltRate = atof(v);
        else if (k == "--fovrate") fovRate = atof(v);
        else if (k == "--also") also.push_back(v);
        else if (k == "--idle") idle = atoi(v);
        else if (k == "--thumb") thumb = atoi(v) != 0;
        else { fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }

    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    for (const std::string& a : also)
    {
        std::shared_ptr<const Nv12Frame> s0, s1;
        auto c = Clip360::open(utf8ToWide(a), &err);
        if (!c || !c->fetch(0, s0, s1, &err))
        {
            fprintf(stderr, "--also %s failed: %s\n", a.c_str(), err.c_str());
            return 1;
        }
    }
    auto clip = Clip360::open(utf8ToWide(path), &err);
    if (!clip)
    {
        fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    const auto& info = clip->info();
    printf("%d frames @ %.3f fps, streams %dx%d %d-bit, gyro %s, tc start %lld\n", info.frames, info.fps(), info.streamW,
           info.streamH, info.bitDepth, clip->hasGyro() ? "yes" : "no", (long long)info.tcStartFrame);

    size_t bytes = size_t(w) * h * 4 * sizeof(float);
    std::vector<float> img(size_t(w) * h * 4);
    if (!gpu.init(bytes))
    {
        fprintf(stderr, "could not set up %s\n", gpu.kind.c_str());
        return 1;
    }
    double decodeMs = 0, renderMs = 0;
    for (int i = 0; i < count; ++i)
    {
        int f = frame + i;
        if (thumb)
        {
            f = info.keyframeAtOrBefore(f);
            printf("thumbnail: frame %d -> keyframe %d\n", frame + i, f);
        }
        auto a = std::chrono::steady_clock::now();
        std::shared_ptr<const Nv12Frame> s0, s1;
        if (!clip->fetch(f, s0, s1, &err, !thumb))
        {
            fprintf(stderr, "decode failed at %d: %s\n", f, err.c_str());
            return 1;
        }
        auto b = std::chrono::steady_clock::now();
        if (!dump.empty()) dumpStreams(dump, *s0, *s1);
        if (s0->index != f || s1->index != f) fprintf(stderr, "frame mismatch: wanted %d got %d/%d\n", f, s0->index, s1->index);
        // pan/tilt/fov animated linearly in time (for motion blur tests)
        auto settingsAt = [&](double dt) {
            RenderSettings r = rs;
            double t = (f - frame) + dt;
            r.pan += panRate * t;
            r.tilt += tiltRate * t;
            r.fovDeg += fovRate * t;
            return r;
        };
        double blurPx = 0;
        std::vector<RfParams> steps = buildRenderSteps(*clip, f, settingsAt, mb, *s0, w, h, w * 4, &blurPx);
        if (mb.enabled && i == 0) printf("motion blur: %.1f px, %zu samples\n", blurPx, steps.size());
        for (int rep = 0; rep < repeat; ++rep)
        {
            bool ok = true;
            if (gpu.kind == "cuda") ok = cudaRender(gpu.stream, steps.data(), int(steps.size()), s0, s1, gpu.dOut, &err);
            else if (gpu.kind == "opencl") ok = openclRender(gpu.queue, steps.data(), int(steps.size()), s0, s1, gpu.clOut, &err);
            else Reprojector(*s0, *s1, steps).renderAll(img.data(), w * 4);
            if (!ok)
            {
                fprintf(stderr, "%s render failed: %s\n", gpu.kind.c_str(), err.c_str());
                return 1;
            }
        }
        auto c = std::chrono::steady_clock::now();
        decodeMs += std::chrono::duration<double, std::milli>(b - a).count();
        renderMs += std::chrono::duration<double, std::milli>(c - b).count() / repeat;
        if (!out.empty())
        {
            if (gpu.kind == "cuda") cudaMemcpy(img.data(), gpu.dOut, bytes, cudaMemcpyDeviceToHost);
            else if (gpu.kind == "opencl") clEnqueueReadBuffer(gpu.queue, gpu.clOut, CL_TRUE, 0, bytes, img.data(), 0, nullptr, nullptr);
            std::string o = out;
            size_t p = o.find("%d");
            if (p != std::string::npos) o.replace(p, 2, std::to_string(f));
            writePpm(o, img, w, h, bits);
        }
    }
    if (idle > 0)
    {
        Sleep(DWORD(idle) * 1000);
        for (int f : {frame + count - 1, frame + count})
        {
            auto a = std::chrono::steady_clock::now();
            std::shared_ptr<const Nv12Frame> s0, s1;
            bool ok = clip->fetch(f, s0, s1, &err);
            printf("after %d s idle: frame %d %s in %.0f ms\n", idle, f, ok ? "decoded" : err.c_str(),
                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        }
    }
    double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("[%s] decode %.1f ms/frame, render %.1f ms/frame (%dx%d), total %.0f ms\n", gpu.kind.c_str(), decodeMs / count,
           renderMs / count, w, h, total);
    return 0;
}
