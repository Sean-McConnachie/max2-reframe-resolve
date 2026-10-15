#include "drt.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "../../src/export360.h"
#include "blob.h"
#include "fusion.h"
#include "gopro_view.h"
#include "util.h"
#include "zip.h"

namespace {

const char* kGoProTool = "ofx.com.gopro.plugins.reframe";
const char* kMax2Id = "com.max2resolve.reframe";
const char* kRenamed = " (Max2 Reframe)";

// Largest differences from the GoPro curves that the keyframe reduction allows.
constexpr double kAngleTol = 0.02;  // degrees (pan, tilt, roll, field of view)
constexpr double kCurvTol = 0.0005;

// ---- XML text helpers (the files are machine written: no comments, no CDATA)

// Finds <tag>text</tag> or <tag/> in [from, to). b and e get the element's start and end.
bool simpleTag(const std::string& s, size_t from, size_t to, const std::string& tag, std::string& text, size_t* b = nullptr,
               size_t* e = nullptr)
{
    const std::string open = "<" + tag + ">", empty = "<" + tag + "/>", close = "</" + tag + ">";
    size_t a = s.find(open, from), z = s.find(empty, from);
    if (z < to && (a == std::string::npos || z < a))
    {
        text.clear();
        if (b) *b = z;
        if (e) *e = z + empty.size();
        return true;
    }
    if (a == std::string::npos || a >= to) return false;
    size_t c = s.find(close, a);
    if (c == std::string::npos || c > to) return false;
    text = s.substr(a + open.size(), c - a - open.size());
    if (b) *b = a;
    if (e) *e = c + close.size();
    return true;
}

std::string unescapeXml(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) return out + s.substr(i);
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#')
        {
            unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp < 0x80) out += char(cp);
            else if (cp < 0x800) { out += char(0xc0 | cp >> 6); out += char(0x80 | (cp & 0x3f)); }
            else if (cp < 0x10000) { out += char(0xe0 | cp >> 12); out += char(0x80 | ((cp >> 6) & 0x3f)); out += char(0x80 | (cp & 0x3f)); }
            else { out += char(0xf0 | cp >> 18); out += char(0x80 | ((cp >> 12) & 0x3f)); out += char(0x80 | ((cp >> 6) & 0x3f)); out += char(0x80 | (cp & 0x3f)); }
        }
        else out += s.substr(i, semi - i + 1);
        i = semi;
    }
    return out;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

double le64double(const std::vector<uint8_t>& b, size_t at)
{
    uint64_t u = 0;
    for (int k = 0; k < 8; ++k) u |= uint64_t(b[at + k]) << (8 * k);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

// "820", "" (zero) or "520|<hex double>" (whole frames and a fraction)
double frameValue(const std::string& s)
{
    if (s.empty()) return 0;
    double v = atof(s.c_str());
    size_t bar = s.find('|');
    std::vector<uint8_t> b;
    if (bar != std::string::npos && fromHex(s.substr(bar + 1), b) && b.size() == 8)
    {
        double f = le64double(b, 0);
        if (f > 0 && f < 1) v += f;
    }
    return v;
}

std::string timecode(double frame, double fps)
{
    int f = int(std::lround(fps));
    if (f <= 0) return std::to_string(long(frame));
    long n = std::lround(frame);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld:%02ld", n / (3600L * f), n / (60L * f) % 60, n / f % 60, n % f);
    return buf;
}

std::string fmt(const char* f, double v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// ---- GoPro Reframe settings of one composition

struct GoProParam
{
    double value = 0;
    bool animated = false;
    Spline spline;
    double at(double frame) const { return animated ? spline.eval(frame) : value; }
};

enum class Read { NotGoPro, Unsupported, Ok };

Read readGoPro(const LuaValue& tools, std::map<std::string, GoProParam>& p, std::string& why)
{
    const LuaValue* gp = nullptr;
    std::string gpName, inName, outName;
    int gopro = 0, mediaIn = 0, mediaOut = 0;
    for (const auto& it : tools.items)
    {
        const std::string& c = it.second.ctor;
        if (c == kGoProTool) gp = &it.second, gpName = it.first.str, ++gopro;
        else if (c == "MediaIn") inName = it.first.str, ++mediaIn;
        else if (c == "MediaOut") outName = it.first.str, ++mediaOut;
    }
    if (!gopro) return Read::NotGoPro;
    if (gopro > 1) return why = "the Fusion composition has more than one GoPro Reframe node", Read::Unsupported;
    if (mediaIn != 1 || mediaOut != 1) return why = "the Fusion composition has more than one MediaIn or MediaOut node", Read::Unsupported;

    auto input = [](const LuaValue& tool, const char* name) -> const LuaValue* {
        const LuaValue* in = tool.get("Inputs");
        return in ? in->get(name) : nullptr;
    };
    auto sourceOp = [](const LuaValue* in) -> std::string {
        const LuaValue* s = in ? in->get("SourceOp") : nullptr;
        return s && s->kind == LuaValue::String ? s->str : std::string();
    };
    const LuaValue* outTool = tools.get(outName);
    if (sourceOp(input(*gp, "Source")) != inName || !outTool || sourceOp(input(*outTool, "Input")) != gpName)
        return why = "the Fusion composition has other nodes between MediaIn and MediaOut", Read::Unsupported;

    static const std::map<std::string, double> defaults = {
        {"pan", 0}, {"tilt", 0}, {"dutch", 0}, {"zoom", 100}, {"distort", 0}, {"yaw", 0}, {"pitch", 0}, {"roll", 0},
        {"xoffset", 0}, {"yoffset", 0}, {"enable_second", 0}, {"apply_svhv", 0}, {"projection", 0}, {"motion_blur", 0},
        {"shutter_angle", 180}};
    std::vector<std::string> usedSplines;
    for (const auto& d : defaults)
    {
        GoProParam& q = p[d.first];
        q.value = d.second;
        const LuaValue* in = input(*gp, d.first.c_str());
        if (!in) continue;
        if (in->get("Expression")) return why = "GoPro setting \"" + d.first + "\" uses an expression", Read::Unsupported;
        std::string op = sourceOp(in);
        if (!op.empty())
        {
            const LuaValue* s = tools.get(op);
            std::string err;
            if (!s || s->ctor != "BezierSpline" || !q.spline.load(*s, &err))
                return why = "GoPro setting \"" + d.first + "\" is animated in a way that the converter cannot read", Read::Unsupported;
            q.animated = true;
            usedSplines.push_back(op);
            continue;
        }
        const LuaValue* v = in->get("Value");
        if (v && v->isNumber()) q.value = v->num;
        else if (v) return why = "GoPro setting \"" + d.first + "\" has an unknown value", Read::Unsupported;
    }
    for (const auto& it : tools.items)
    {
        const std::string& c = it.second.ctor;
        if (c == kGoProTool || c == "MediaIn" || c == "MediaOut" || c == "AudioDisplay") continue;
        if (c == "BezierSpline" && std::find(usedSplines.begin(), usedSplines.end(), it.first.str) != usedSplines.end()) continue;
        return why = "the Fusion composition has other nodes (" + (c.empty() ? it.first.str : c) + ")", Read::Unsupported;
    }
    for (const char* n : {"yaw", "pitch", "roll", "xoffset", "yoffset"})
        if (p[n].animated || std::fabs(p[n].value) > 1e-9)
            return why = std::string("GoPro setting \"") + n + "\" is not zero", Read::Unsupported;
    if (p["enable_second"].value != 0) return why = "GoPro Second Camera is on", Read::Unsupported;
    if (p["apply_svhv"].animated || p["apply_svhv"].value != 0) return why = "GoPro SuperView/HyperView is on", Read::Unsupported;
    return Read::Ok;
}

// ---- keyframes

struct Key
{
    int frame;  // timeline frames from the clip's first frame
    double v;
};

// The fewest keyframes (linear between them) that stay within `tol` of every frame's value.
// Empty: the value does not change.
std::vector<Key> reduce(const std::vector<double>& v, double tol)
{
    std::vector<Key> keys;
    bool constant = true;
    for (double x : v) constant &= std::fabs(x - v[0]) <= tol;
    if (constant) return keys;
    const int n = int(v.size());
    keys.push_back({0, v[0]});
    int i = 0;
    while (i < n - 1)
    {
        int j = i + 1;
        while (j + 1 < n)
        {
            bool ok = true;
            for (int k = i + 1; k <= j && ok; ++k)
                ok = std::fabs(v[k] - (v[i] + (v[j + 1] - v[i]) * double(k - i) / double(j + 1 - i))) <= tol;
            if (!ok) break;
            ++j;
        }
        keys.push_back({j, v[j]});
        i = j;
    }
    return keys;
}

// ---- the Max2 Reframe effect, in the form Resolve stores Edit page OpenFX effects

PbWriter doubleParam(const std::string& name, const std::vector<double>& v, double tol)
{
    PbWriter p;
    p.str(1, name);
    std::vector<Key> keys = reduce(v, tol);
    p.msg(2, PbWriter().dbl(2, v[0]));
    for (const Key& k : keys)
    {
        PbWriter kf, val;
        if (k.frame) kf.varint(1, uint64_t(k.frame) * 2);  // Resolve counts keyframe times in half frames
        val.msg(1, PbWriter().dbl(2, k.v));
        val.varint(2, 1);
        val.bytes(4, {});
        kf.msg(2, val);
        p.msg(3, kf);
    }
    return p;
}

PbWriter intParam(const std::string& name, int v)
{
    PbWriter p;
    p.str(1, name);
    p.msg(2, PbWriter().varint(3, uint64_t(v)));
    return p;
}

std::vector<uint8_t> max2Effect(const std::vector<std::pair<std::string, PbWriter>>& params)
{
    // Field numbers and constants as Resolve 21.1 writes them for this plugin.
    const uint64_t kPluginTag = 5195353;
    PbWriter plugin;
    plugin.varint(1, kPluginTag).str(2, kMax2Id).str(3, "OfxImageEffectContextFilter").varint(4, 1);
    for (const auto& p : params) plugin.msg(5, p.second);
    PbWriter ref;
    ref.msg(2, PbWriter().varint(1, kPluginTag).str(2, kMax2Id).str(3, "OfxImageEffectContextFilter"));
    PbWriter body;
    body.varint(1, 109).varint(2, 0).msg(3, PbWriter().msg(2, plugin)).msg(4, ref).varint(9, 0);
    PbWriter entry;
    entry.varint(1, 50).varint(3, 4).varint(7, 2).varint(8, 362458336999ull).msg(9, body);
    PbWriter top;
    top.msg(1, entry);
    return top.data();
}

struct Timeline
{
    double fps = 0;
    int w = 0, h = 0;
};

bool sequenceInfo(const std::string& mp, const std::string& id, Timeline& t)
{
    size_t a = mp.find("<Sm2Sequence DbId=\"" + id + "\"");
    if (a == std::string::npos) return false;
    size_t z = mp.find("</Sm2Sequence>", a);
    std::string fr, res;
    std::vector<uint8_t> b;
    if (!simpleTag(mp, a, z, "FrameRate", fr) || !fromHex(fr, b) || b.size() < 8) return false;
    t.fps = le64double(b, 0);
    if (!simpleTag(mp, a, z, "Resolution", res) || !fromHex(res, b) || b.size() < 16) return false;
    t.w = int(be32(&b[4]));
    t.h = int(be32(&b[12]));
    return t.fps > 0 && t.w > 0 && t.h > 0;
}

struct ClipContext
{
    std::string name;
    double start = 0, in = 0, duration = 0, mediaFps = 0;
    std::wstring path;
};

// Converts one clip element. Returns the new element text, or an empty string if the clip stays as it is.
std::string convertClip(const std::string& el, const Timeline& tl, ConvertResult& res, std::map<std::wstring, std::pair<bool, std::string>>& sources)
{
    size_t cb, ce;
    std::string compTable;
    if (!simpleTag(el, 0, el.size(), "CompositionTable", compTable, &cb, &ce)) return {};
    std::string hex;
    if (!simpleTag(compTable, 0, compTable.size(), "CompositionBA", hex) || hex.empty()) return {};

    ClipContext c;
    std::string v;
    simpleTag(el, 0, el.size(), "Name", v);
    c.name = unescapeXml(v);
    if (simpleTag(el, 0, el.size(), "Start", v)) c.start = frameValue(v);
    std::string where = c.name + " at " + timecode(c.start, tl.fps);

    LuaValue tools;
    std::string why;
    if (!decodeComposition(hex, tools, &why))
    {
        // It may or may not use GoPro Reframe; say so, so that no GoPro clip is left without a word.
        res.notes.push_back(where + ": the Fusion composition cannot be read (" + why + ").");
        return {};
    }
    std::map<std::string, GoProParam> p;
    Read r = readGoPro(tools, p, why);
    if (r == Read::NotGoPro) return {};
    ++res.goproClips;
    auto skip = [&](const std::string& reason) {
        res.notes.push_back(where + ": " + reason + ".");
        return std::string();
    };
    if (r == Read::Unsupported) return skip(why);

    const int projection = int(p["projection"].value);
    const int pw = projection == 7 ? 3840 : projection == 2 ? 1920 : 0, ph = projection == 7 ? 2160 : projection == 2 ? 1080 : 0;
    if (p["projection"].animated || !pw || pw != tl.w || ph != tl.h)
        return skip("the GoPro output size is not the same as the timeline size (" + std::to_string(tl.w) + " x " + std::to_string(tl.h) + ")");

    if (simpleTag(el, 0, el.size(), "In", v)) c.in = frameValue(v);
    if (simpleTag(el, 0, el.size(), "Duration", v)) c.duration = frameValue(v);
    std::vector<uint8_t> b;
    if (simpleTag(el, 0, el.size(), "MediaFrameRate", v) && fromHex(v, b) && b.size() >= 8) c.mediaFps = le64double(b, 0);
    if (c.mediaFps <= 0) c.mediaFps = tl.fps;
    if (!simpleTag(el, 0, el.size(), "MediaFilePath", v) || v.empty()) return skip("the clip has no file");
    c.path = utf8ToWide(unescapeXml(v));

    // The plugin opens the .360 file itself; make sure it will find it.
    auto found = sources.find(c.path);
    if (found == sources.end())
    {
        std::wstring source;
        std::string whyNot;
        bool ok = findSource360ForExport(c.path, source, &whyNot);
        found = sources.emplace(c.path, std::make_pair(ok, ok ? wideToUtf8(source) : whyNot)).first;
    }
    if (!found->second.first) return skip("no .360 file found (" + found->second.second + ")");

    // Values for each timeline frame of the clip. Fusion counts frames of the source file from its start.
    const char* animatedNames[] = {"pan", "tilt", "dutch", "zoom", "distort"};
    bool animated = false;
    for (const char* n : animatedNames) animated |= p[n].animated;
    const int frames = animated ? std::max(1, int(std::lround(c.duration))) : 1;
    std::vector<double> pan(frames), tilt(frames), roll(frames), fov(frames), curv(frames), errMax(frames);
    auto sample = [&](double fixedCurv) {
        std::map<std::pair<long long, long long>, Max2Lens> lensCache;
        Max2Lens prev;
        bool havePrev = false;
        for (int t = 0; t < frames; ++t)
        {
            double f = (c.in + t) * c.mediaFps / tl.fps;
            pan[t] = p["pan"].at(f);
            tilt[t] = p["tilt"].at(f);
            roll[t] = p["dutch"].at(f);
            double zoom = p["zoom"].at(f), lc = p["distort"].at(f);
            auto key = std::make_pair(std::llround(zoom * 1000), std::llround(lc * 1000));
            auto it = lensCache.find(key);
            if (it == lensCache.end()) it = lensCache.emplace(key, fitLens(zoom, lc, havePrev ? &prev : nullptr, fixedCurv)).first;
            prev = it->second;
            havePrev = true;
            fov[t] = prev.fov;
            curv[t] = prev.curv;
            errMax[t] = prev.maxErr;
        }
    };
    sample(-1);
    // A curvature that only wanders a little over the clip is held at one value, with the field of view taking
    // up the difference, if that moves no part of any frame by more than a pixel.
    const std::vector<double> freeErr = errMax;
    std::vector<double> values = curv;
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    if (values.size() > 1 && values.back() - values.front() <= 0.02)
    {
        double bestCurv = -1, bestCost = 1e300;
        for (double v : values)
        {
            sample(v);
            double worse = 0;
            for (int t = 0; t < frames; ++t) worse = std::max(worse, errMax[t] - freeErr[t]);
            if (worse <= 1.0 && worse < bestCost) bestCost = worse, bestCurv = v;
        }
        sample(bestCurv);
    }

    double shutter = p["shutter_angle"].at(c.in * c.mediaFps / tl.fps);
    bool blur = p["motion_blur"].at(c.in * c.mediaFps / tl.fps) != 0;
    std::vector<std::pair<std::string, PbWriter>> params;
    params.emplace_back("ResolveUseAlphaForTrackCompositing", intParam("ResolveUseAlphaForTrackCompositing", 1));
    params.emplace_back("curv", doubleParam("curv", curv, kCurvTol));
    params.emplace_back("fov", doubleParam("fov", fov, kAngleTol));
    if (blur) params.emplace_back("motionBlur", intParam("motionBlur", 1));
    params.emplace_back("pan", doubleParam("pan", pan, kAngleTol));
    params.emplace_back("roll", doubleParam("roll", roll, kAngleTol));
    if (blur && std::fabs(shutter - 180) > 1e-9) params.emplace_back("shutter", doubleParam("shutter", {shutter}, kAngleTol));
    params.emplace_back("tilt", doubleParam("tilt", tilt, kAngleTol));
    std::sort(params.begin(), params.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::string eff;
    size_t eb, ee;
    if (!simpleTag(el, 0, el.size(), "EffectFiltersBA", eff, &eb, &ee)) return skip("the clip has no effect list");
    std::vector<uint8_t> list;
    if (!decodeResolveBlob(eff, list, &why)) return skip(why);
    std::vector<uint8_t> mine = max2Effect(params);
    list.insert(list.end(), mine.begin(), mine.end());

    // New element text: the effect list replaced, the composition removed (with the indentation before it).
    size_t lineStart = el.rfind('\n', cb);
    lineStart = lineStart == std::string::npos ? cb : lineStart;
    std::string out;
    out += el.substr(0, eb);
    out += "<EffectFiltersBA>" + encodeResolveBlob(list) + "</EffectFiltersBA>";
    if (ee <= lineStart)
    {
        out += el.substr(ee, lineStart - ee);
        out += el.substr(ce);
    }
    else
    {
        // the composition comes first (not seen in Resolve 21.1 files)
        return skip("the clip layout is unknown");
    }

    std::string line = where + ": source " + found->second.second;
    auto keyCount = [&](const std::vector<double>& x, double tol) { return std::to_string(reduce(x, tol).size()); };
    line += "; pan " + fmt("%.3f", pan[0]) + " tilt " + fmt("%.3f", tilt[0]) + " roll " + fmt("%.3f", roll[0]) + " fov " +
            fmt("%.3f", fov[0]) + " curv " + fmt("%.4f", curv[0]) + " blur " + (blur ? "on" : "off");
    if (animated)
        line += "; keyframes pan " + keyCount(pan, kAngleTol) + " tilt " + keyCount(tilt, kAngleTol) + " roll " +
                keyCount(roll, kAngleTol) + " fov " + keyCount(fov, kAngleTol) + " curv " + keyCount(curv, kCurvTol);
    line += "; lens offset max " + fmt("%.1f", *std::max_element(errMax.begin(), errMax.end())) + " px";
    res.log.push_back(line);
    std::pair<std::string, std::string> src(wideToUtf8(c.path), found->second.second);
    if (std::find(res.sources.begin(), res.sources.end(), src) == res.sources.end()) res.sources.push_back(src);
    ++res.converted;
    return out;
}

void renameTimelines(std::string& mp, ConvertResult& res)
{
    for (const char* owner : {"<Sm2MpTimelineClip ", "<Sm2Timeline "})
    {
        for (size_t at = mp.find(owner); at != std::string::npos; at = mp.find(owner, at + 1))
        {
            size_t nb, ne;
            std::string name;
            if (!simpleTag(mp, at, mp.size(), "Name", name, &nb, &ne)) continue;
            if (name.size() >= strlen(kRenamed) && name.compare(name.size() - strlen(kRenamed), std::string::npos, kRenamed) == 0) continue;
            mp.replace(nb, ne - nb, "<Name>" + name + kRenamed + "</Name>");
            res.timeline = unescapeXml(name) + kRenamed;
        }
    }
}

} // namespace

bool convertTimelineFile(const std::wstring& in, const std::wstring& outPath, ConvertResult& res, std::string* err)
{
    std::vector<ZipEntry> entries;
    if (!readZip(in, entries, err)) return false;
    ZipEntry* mpEntry = nullptr;
    for (auto& e : entries)
        if (e.name.size() >= 12 && e.name.compare(e.name.size() - 12, 12, "MpFolder.xml") == 0) mpEntry = &e;
    if (!mpEntry)
    {
        if (err) *err = "the file is not a DaVinci Resolve timeline (.drt)";
        return false;
    }
    std::string mp(mpEntry->data.begin(), mpEntry->data.end());

    std::map<std::wstring, std::pair<bool, std::string>> sources;
    bool anyTimeline = false;
    for (auto& e : entries)
    {
        if (e.name.rfind("SeqContainer/", 0) != 0) continue;
        std::string s(e.data.begin(), e.data.end());
        std::string seq;
        Timeline tl;
        if (!simpleTag(s, 0, s.size(), "Sequence", seq) || !sequenceInfo(mp, seq, tl))
        {
            if (err) *err = "the timeline settings cannot be read";
            return false;
        }
        anyTimeline = true;
        std::string out;
        size_t pos = 0;
        const std::string open = "<Sm2TiVideoClip DbId=", close = "</Sm2TiVideoClip>";
        for (size_t a = s.find(open); a != std::string::npos; a = s.find(open, pos))
        {
            size_t z = s.find(close, a);
            if (z == std::string::npos) break;
            z += close.size();
            out += s.substr(pos, a - pos);
            std::string el = s.substr(a, z - a);
            std::string conv = convertClip(el, tl, res, sources);
            out += conv.empty() ? el : conv;
            pos = z;
        }
        out += s.substr(pos);
        e.data.assign(out.begin(), out.end());
    }
    if (!anyTimeline)
    {
        if (err) *err = "the file has no timeline";
        return false;
    }
    if (res.converted == 0) return true;  // nothing changed: no new file
    renameTimelines(mp, res);
    mpEntry->data.assign(mp.begin(), mp.end());
    return writeZip(outPath, entries, err);
}
