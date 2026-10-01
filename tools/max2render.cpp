// Command-line test harness for the reframe engine (no Resolve needed).
//   max2render file.360 [--frame N] [--count N] [--out out.ppm] [--w 1920] [--h 1080] [--proj lens|erp]
//              [--fov 100] [--curv 0.25] [--pan 0] [--tilt 0] [--roll 0] [--stab 1] [--horizon 1] [--dirlock 0]
//              [--smooth 0.3] [--ss 1]
// With --count > 1 it renders a sequence and reports timing; %d in --out is replaced by the frame number.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/engine.h"

static void writePpm(const std::string& path, const std::vector<float>& rgba, int w, int h)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                row[size_t(x) * 3 + c] = uint8_t(std::clamp(rgba[(size_t(y) * w + x) * 4 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: max2render file.360 [options]\n");
        return 2;
    }
    std::string path = argv[1];
    int frame = 0, count = 1, w = 1920, h = 1080;
    std::string out, dump;
    int threadsOpt = 0, repeat = 1;
    RenderSettings rs;
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
        else if (k == "--dump") dump = v;
        else if (k == "--threads") threadsOpt = atoi(v);
        else if (k == "--repeat") repeat = atoi(v);
        else { fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }

    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    auto clip = Clip360::open(utf8ToWide(path), &err);
    if (!clip)
    {
        fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    const auto& info = clip->info();
    printf("%d frames @ %.3f fps, streams %dx%d, gyro %s, tc start %lld\n", info.frames, info.fps(), info.streamW,
           info.streamH, clip->hasGyro() ? "yes" : "no", (long long)info.tcStartFrame);

    unsigned nThreads = threadsOpt > 0 ? unsigned(threadsOpt) : std::max(1u, std::thread::hardware_concurrency());
    std::vector<float> img(size_t(w) * h * 4);
    double decodeMs = 0, renderMs = 0;
    for (int i = 0; i < count; ++i)
    {
        int f = frame + i;
        auto a = std::chrono::steady_clock::now();
        std::shared_ptr<const Nv12Frame> s0, s1;
        if (!clip->fetch(f, s0, s1, &err))
        {
            fprintf(stderr, "decode failed at %d: %s\n", f, err.c_str());
            return 1;
        }
        auto b = std::chrono::steady_clock::now();
        if (!dump.empty())
            for (int k = 0; k < 2; ++k)
            {
                const Nv12Frame& fr = k ? *s1 : *s0;
                FILE* df = fopen((dump + std::to_string(k) + ".pgm").c_str(), "wb");
                fprintf(df, "P5\n%d %d\n255\n", fr.width, fr.height);
                for (int y = 0; y < fr.height; ++y) fwrite(fr.y() + size_t(y) * fr.pitch, 1, fr.width, df);
                fclose(df);
            }
        if (s0->index != f || s1->index != f) fprintf(stderr, "frame mismatch: wanted %d got %d/%d\n", f, s0->index, s1->index);
        Reprojector rp(*s0, *s1, clip->view(f, rs), w, h);
        for (int rep = 0; rep < repeat; ++rep) {
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nThreads; ++t)
            pool.emplace_back([&, t] {
                for (int y = int(t); y < h; y += int(nThreads)) rp.renderRow(y, 0, w, img.data() + size_t(y) * w * 4);
            });
        for (auto& th : pool) th.join();
        }
        auto c = std::chrono::steady_clock::now();
        decodeMs += std::chrono::duration<double, std::milli>(b - a).count();
        renderMs += std::chrono::duration<double, std::milli>(c - b).count() / repeat;
        if (!out.empty())
        {
            std::string o = out;
            size_t p = o.find("%d");
            if (p != std::string::npos) o.replace(p, 2, std::to_string(f));
            writePpm(o, img, w, h);
        }
    }
    double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("decode %.1f ms/frame, render %.1f ms/frame (%dx%d, %u threads), total %.0f ms\n", decodeMs / count,
           renderMs / count, w, h, nThreads, total);
    return 0;
}
