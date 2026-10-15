// Test program for the timeline converter. tools\test\test_convert.py runs it.
//   convert_test angle <zoom> <lensCurve> <r>...        GoPro view angles (degrees) at pixel distances r
//   convert_test max2angle <fov> <curv> <r>...           Max2 view angles
//   convert_test fit <zoom> <lensCurve>                  fitted Max2 lens: fov curv meanErr maxErr
//   convert_test splines <file.drt>                      every GoPro curve: clip, setting, then "t=value" samples
//   convert_test effects <file.drt>                      every clip's effect list, decoded (protocol buffer fields)
//   convert_test convert <in.drt> <out.drt>              as Max2Convert.exe, with the report on stdout
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blob.h"
#include "drt.h"
#include "fusion.h"
#include "gopro_view.h"
#include "zip.h"

namespace {

std::wstring widen(const char* s)
{
    std::wstring w;
    for (; *s; ++s) w += wchar_t((unsigned char)*s);
    return w;
}

std::vector<std::string> clipElements(const std::wstring& path)
{
    std::vector<ZipEntry> entries;
    std::string err;
    std::vector<std::string> out;
    if (!readZip(path, entries, &err))
    {
        fprintf(stderr, "read failed: %s\n", err.c_str());
        exit(1);
    }
    for (const auto& e : entries)
    {
        if (e.name.rfind("SeqContainer/", 0) != 0) continue;
        std::string s(e.data.begin(), e.data.end());
        const std::string open = "<Sm2TiVideoClip DbId=", close = "</Sm2TiVideoClip>";
        for (size_t a = s.find(open); a != std::string::npos; a = s.find(open, a + 1))
        {
            size_t z = s.find(close, a);
            out.push_back(s.substr(a, z + close.size() - a));
        }
    }
    return out;
}

std::string tag(const std::string& el, const std::string& t)
{
    size_t a = el.find("<" + t + ">");
    if (a == std::string::npos) return {};
    size_t b = el.find("</" + t + ">", a);
    return el.substr(a + t.size() + 2, b - a - t.size() - 2);
}

void dumpPb(const std::vector<uint8_t>& d, int depth)
{
    std::vector<PbField> f;
    if (!parsePb(d.data(), d.size(), f)) return;
    for (const auto& x : f)
    {
        printf("%*s%u:", depth * 2, "", x.num);
        if (x.wire == 2)
        {
            std::vector<PbField> sub;
            bool printable = !x.bytes.empty();
            for (uint8_t c : x.bytes) printable &= c >= 32 && c < 127;
            if (printable) printf(" \"%.*s\"\n", int(x.bytes.size()), reinterpret_cast<const char*>(x.bytes.data()));
            else if (!x.bytes.empty() && parsePb(x.bytes.data(), x.bytes.size(), sub))
            {
                printf("\n");
                dumpPb(x.bytes, depth + 1);
            }
            else printf(" bytes[%zu]\n", x.bytes.size());
        }
        else if (x.wire == 1)
        {
            double v;
            memcpy(&v, &x.value, 8);
            printf(" d=%.10g\n", v);
        }
        else printf(" %llu\n", (unsigned long long)x.value);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    std::string cmd = argv[1];
    if (cmd == "angle" || cmd == "max2angle")
    {
        double a = atof(argv[2]), b = atof(argv[3]);
        for (int i = 4; i < argc; ++i)
            printf("%.6f\n", cmd == "angle" ? goproAngle(atof(argv[i]), a, b) : max2Angle(atof(argv[i]), a, b));
        return 0;
    }
    if (cmd == "fit")
    {
        Max2Lens l = fitLens(atof(argv[2]), atof(argv[3]));
        printf("%.6f %.6f %.6f %.6f\n", l.fov, l.curv, l.meanErr, l.maxErr);
        return 0;
    }
    if (cmd == "splines")
    {
        int index = 0;
        for (const auto& el : clipElements(widen(argv[2])))
        {
            ++index;
            std::string hex = tag(el, "CompositionBA");
            if (hex.empty()) continue;
            LuaValue tools;
            std::string err;
            if (!decodeComposition(hex, tools, &err)) continue;
            for (const auto& it : tools.items)
            {
                if (it.second.ctor != "BezierSpline") continue;
                Spline s;
                if (!s.load(it.second, &err)) continue;
                printf("%d %s", index, it.first.str.c_str());
                for (double t = s.firstTime() - 3; t <= s.lastTime() + 3; t += 0.5) printf(" %g=%.9g", t, s.eval(t));
                printf("\n");
            }
        }
        return 0;
    }
    if (cmd == "effects")
    {
        int index = 0;
        for (const auto& el : clipElements(widen(argv[2])))
        {
            ++index;
            size_t a = el.find("<EffectFiltersBA>");
            if (a == std::string::npos) continue;
            std::string hex = tag(el, "EffectFiltersBA");
            std::vector<uint8_t> d;
            std::string err;
            if (!decodeResolveBlob(hex, d, &err))
            {
                printf("clip %d: %s\n", index, err.c_str());
                continue;
            }
            printf("clip %d %s\n", index, tag(el, "Name").c_str());
            dumpPb(d, 1);
        }
        return 0;
    }
    if (cmd == "convert")
    {
        ConvertResult r;
        std::string err;
        bool ok = convertTimelineFile(widen(argv[2]), widen(argv[3]), r, &err);
        printf("ok %d\ngopro %d\nconverted %d\ntimeline %s\n", ok, r.goproClips, r.converted, r.timeline.c_str());
        if (!ok) printf("error %s\n", err.c_str());
        for (const auto& n : r.notes) printf("note %s\n", n.c_str());
        for (const auto& l : r.log) printf("clip %s\n", l.c_str());
        return ok ? 0 : 1;
    }
    return 2;
}
