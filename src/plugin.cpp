// OpenFX plugin: reframes GoPro Max/Max 2 .360 clips straight from the original file, with gyro stabilization.
//
// Resolve hands OFX effects images at timeline resolution, so instead of using the input image this plugin
// reads the clip's source path (kOfxImageEffectPropSrcFilePath) and decodes the .360 itself at full
// resolution. The input image is only used as a pass-through when the source is not a .360 file.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include "engine.h"
#include "log.h"

#define kPluginName "Max2 Reframe"
#define kPluginGrouping "GoPro 360"
#define kPluginDescription \
    "Reframes GoPro Max / Max 2 .360 clips directly from the original file at full resolution, " \
    "with gyro stabilization, horizon lock and direction lock."
#define kPluginIdentifier "com.max2resolve.reframe"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

namespace {

class ReframeProcessor : public OFX::ImageProcessor
{
public:
    ReframeProcessor(OFX::ImageEffect& effect, const Reprojector& rp, const OfxRectI& bounds)
        : OFX::ImageProcessor(effect), m_Rp(rp), m_Bounds(bounds)
    {
    }

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y)
        {
            if (_effect.abort()) break;
            float* d = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            if (!d) continue;
            int yTop = m_Bounds.y2 - 1 - y;  // OFX rows go bottom-up
            m_Rp.renderRow(yTop, win.x1 - m_Bounds.x1, win.x2 - m_Bounds.x1, d);
        }
    }

private:
    const Reprojector& m_Rp;
    OfxRectI m_Bounds;
};

class CopyProcessor : public OFX::ImageProcessor
{
public:
    CopyProcessor(OFX::ImageEffect& effect, OFX::Image* src) : OFX::ImageProcessor(effect), m_Src(src) {}

    void multiThreadProcessImages(OfxRectI win) override
    {
        for (int y = win.y1; y < win.y2; ++y)
        {
            float* d = static_cast<float*>(_dstImg->getPixelAddress(win.x1, y));
            for (int x = win.x1; x < win.x2; ++x, d += 4)
            {
                const float* s = m_Src ? static_cast<const float*>(m_Src->getPixelAddress(x, y)) : nullptr;
                if (s) std::memcpy(d, s, 4 * sizeof(float));
                else d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }

private:
    OFX::Image* m_Src;
};

std::atomic<int> g_RenderLogBudget{40};

} // namespace

class Max2ReframePlugin : public OFX::ImageEffect
{
public:
    explicit Max2ReframePlugin(OfxImageEffectHandle handle) : ImageEffect(handle)
    {
        m_Dst = fetchClip(kOfxImageEffectOutputClipName);
        m_Src = fetchClip(kOfxImageEffectSimpleSourceClipName);
        m_Stabilize = fetchBooleanParam("stabilize");
        m_Horizon = fetchBooleanParam("horizon");
        m_DirLock = fetchBooleanParam("dirlock");
        m_Smooth = fetchDoubleParam("smooth");
        m_Pan = fetchDoubleParam("pan");
        m_Tilt = fetchDoubleParam("tilt");
        m_Roll = fetchDoubleParam("roll");
        m_Fov = fetchDoubleParam("fov");
        m_Curv = fetchDoubleParam("curv");
        m_Proj = fetchChoiceParam("proj");
        m_Quality = fetchChoiceParam("quality");
        m_FrameOffset = fetchIntParam("frameOffset");
        m_Override = fetchStringParam("sourceOverride");
        m_SrcPath = readSrcPath();
        logf("instance created, source path \"%s\"", m_SrcPath.c_str());
    }

    void render(const OFX::RenderArguments& args) override
    {
        std::unique_ptr<OFX::Image> dst(m_Dst->fetchImage(args.time));
        if (!dst || dst->getPixelDepth() != OFX::eBitDepthFloat || dst->getPixelComponents() != OFX::ePixelComponentRGBA)
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

        std::string path;
        m_Override->getValue(path);
        if (path.empty())
        {
            // Resolve can change the source of an existing instance, so ask on every render.
            std::string p = readSrcPath();
            if (!p.empty() && p != m_SrcPath)
            {
                if (!m_SrcPath.empty()) logf("source path changed to \"%s\"", p.c_str());
                m_SrcPath = p;
            }
            path = m_SrcPath;
        }
        std::string err;
        std::shared_ptr<Clip360> clip = path.empty() ? nullptr : Clip360::open(utf8ToWide(path), &err);
        if (!clip)
        {
            if (g_RenderLogBudget-- > 0)
                logf("render t=%.2f: no usable .360 source (\"%s\": %s), passing input through", args.time, path.c_str(),
                     path.empty() ? "host gave no source path" : err.c_str());
            passThrough(args, dst.get());
            return;
        }

        const auto& info = clip->info();
        double hostFrame = args.srcFrame >= 0 ? double(args.srcFrame) : args.time;
        int frame = resolveSourceFrame(info, hostFrame) + m_FrameOffset->getValueAtTime(args.time);
        frame = std::clamp(frame, 0, info.frames - 1);

        RenderSettings rs;
        rs.stab.stabilize = m_Stabilize->getValueAtTime(args.time);
        rs.stab.horizon = m_Horizon->getValueAtTime(args.time);
        rs.stab.directionLock = m_DirLock->getValueAtTime(args.time);
        rs.stab.smoothSeconds = m_Smooth->getValueAtTime(args.time);
        rs.pan = m_Pan->getValueAtTime(args.time);
        rs.tilt = m_Tilt->getValueAtTime(args.time);
        rs.roll = m_Roll->getValueAtTime(args.time);
        rs.fovDeg = m_Fov->getValueAtTime(args.time);
        rs.curvature = m_Curv->getValueAtTime(args.time);
        int proj = 0, quality = 0;
        m_Proj->getValueAtTime(args.time, proj);
        m_Quality->getValueAtTime(args.time, quality);
        rs.proj = proj == 1 ? Projection::Equirectangular : Projection::Lens;
        rs.supersample = quality == 1 ? 2 : 1;

        std::shared_ptr<const Nv12Frame> s0, s1;
        if (!clip->fetch(frame, s0, s1, &err))
        {
            logf("render t=%.2f frame %d: decode failed: %s", args.time, frame, err.c_str());
            passThrough(args, dst.get());
            return;
        }

        OfxRectI b = dst->getBounds();
        if (g_RenderLogBudget-- > 0)
            logf("render t=%.2f srcFrame=%d -> frame %d (got %d/%d), out %dx%d window %d,%d-%d,%d scale %.3f draft=%d",
                 args.time, args.srcFrame, frame, s0->index, s1->index, b.x2 - b.x1, b.y2 - b.y1, args.renderWindow.x1,
                 args.renderWindow.y1, args.renderWindow.x2, args.renderWindow.y2, args.renderScale.x,
                 int(args.renderQualityDraft));

        Reprojector rp(*s0, *s1, clip->view(frame, rs), b.x2 - b.x1, b.y2 - b.y1);
        ReframeProcessor proc(*this, rp, b);
        proc.setDstImg(dst.get());
        proc.setRenderWindow(args.renderWindow);
        proc.process();
    }

    bool isIdentity(const OFX::IsIdentityArguments&, OFX::Clip*&, double&) override { return false; }

private:
    std::string readSrcPath()
    {
        try
        {
            return getPropertySet().propGetString(kOfxImageEffectPropSrcFilePath, false);
        }
        catch (...)
        {
            return {};
        }
    }

    void passThrough(const OFX::RenderArguments& args, OFX::Image* dst)
    {
        std::unique_ptr<OFX::Image> src(m_Src && m_Src->isConnected() ? m_Src->fetchImage(args.time) : nullptr);
        CopyProcessor proc(*this, src.get());
        proc.setDstImg(dst);
        proc.setRenderWindow(args.renderWindow);
        proc.process();
    }

    OFX::Clip* m_Dst;
    OFX::Clip* m_Src;
    OFX::BooleanParam *m_Stabilize, *m_Horizon, *m_DirLock;
    OFX::DoubleParam *m_Smooth, *m_Pan, *m_Tilt, *m_Roll, *m_Fov, *m_Curv;
    OFX::ChoiceParam *m_Proj, *m_Quality;
    OFX::IntParam* m_FrameOffset;
    OFX::StringParam* m_Override;
    std::string m_SrcPath;
};

using namespace OFX;

class Max2ReframeFactory : public OFX::PluginFactoryHelper<Max2ReframeFactory>
{
public:
    Max2ReframeFactory() : OFX::PluginFactoryHelper<Max2ReframeFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor) {}

    void describe(OFX::ImageEffectDescriptor& d) override
    {
        d.setLabels(kPluginName, kPluginName, kPluginName);
        d.setPluginGrouping(kPluginGrouping);
        d.setPluginDescription(kPluginDescription);
        d.addSupportedContext(eContextFilter);
        d.addSupportedContext(eContextGeneral);
        d.addSupportedBitDepth(eBitDepthFloat);
        d.setSingleInstance(false);
        d.setHostFrameThreading(false);
        d.setSupportsMultiResolution(false);
        d.setSupportsTiles(false);
        d.setTemporalClipAccess(false);
        d.setRenderTwiceAlways(false);
        d.setSupportsMultipleClipPARs(false);
        d.setRenderThreadSafety(eRenderInstanceSafe);
    }

    void describeInContext(OFX::ImageEffectDescriptor& d, OFX::ContextEnum) override
    {
        ClipDescriptor* src = d.defineClip(kOfxImageEffectSimpleSourceClipName);
        src->addSupportedComponent(ePixelComponentRGBA);
        src->setTemporalClipAccess(false);
        src->setSupportsTiles(false);
        src->setIsMask(false);
        ClipDescriptor* dst = d.defineClip(kOfxImageEffectOutputClipName);
        dst->addSupportedComponent(ePixelComponentRGBA);
        dst->setSupportsTiles(false);

        PageParamDescriptor* page = d.definePageParam("Controls");

        GroupParamDescriptor* gView = d.defineGroupParam("viewGroup");
        gView->setLabels("View", "View", "View");
        GroupParamDescriptor* gStab = d.defineGroupParam("stabGroup");
        gStab->setLabels("Stabilization", "Stabilization", "Stabilization");
        GroupParamDescriptor* gAdv = d.defineGroupParam("advGroup");
        gAdv->setLabels("Advanced", "Advanced", "Advanced");
        gAdv->setOpen(false);

        auto dbl = [&](const char* name, const char* label, const char* hint, double def, double lo, double hi,
                       double dlo, double dhi, GroupParamDescriptor* g) {
            DoubleParamDescriptor* p = d.defineDoubleParam(name);
            p->setLabels(label, label, label);
            p->setHint(hint);
            p->setDefault(def);
            p->setRange(lo, hi);
            p->setDisplayRange(dlo, dhi);
            p->setIncrement(0.1);
            p->setAnimates(true);
            p->setParent(*g);
            page->addChild(*p);
            return p;
        };
        auto boolean = [&](const char* name, const char* label, const char* hint, bool def, GroupParamDescriptor* g) {
            BooleanParamDescriptor* p = d.defineBooleanParam(name);
            p->setLabels(label, label, label);
            p->setHint(hint);
            p->setDefault(def);
            p->setAnimates(false);
            p->setParent(*g);
            page->addChild(*p);
            return p;
        };

        dbl("pan", "Pan", "Look left (-) / right (+), degrees", 0, -3600, 3600, -180, 180, gView);
        dbl("tilt", "Tilt", "Look down (-) / up (+), degrees", 0, -90, 90, -90, 90, gView);
        dbl("roll", "Roll", "Rotate the view, degrees", 0, -180, 180, -45, 45, gView);
        dbl("fov", "Field of View", "Horizontal field of view, degrees", 100, 5, 340, 20, 170, gView);
        dbl("curv", "Lens Curvature", "0 = rectilinear (straight lines), 1 = stereographic (wide, tiny-planet)", 0.25, 0, 1, 0, 1, gView);
        {
            ChoiceParamDescriptor* p = d.defineChoiceParam("proj");
            p->setLabels("Projection", "Projection", "Projection");
            p->setHint("Flat reframed view, or the full stabilized 360 sphere (2:1)");
            p->appendOption("Flat");
            p->appendOption("Equirectangular 360");
            p->setDefault(0);
            p->setAnimates(false);
            p->setParent(*gView);
            page->addChild(*p);
        }

        boolean("stabilize", "Stabilize", "Remove camera rotation using the gyro data in the .360 file", true, gStab);
        boolean("horizon", "Horizon Lock", "Keep the horizon level", true, gStab);
        boolean("dirlock", "Direction Lock",
                "On: the view keeps pointing the same way in the world when the camera turns. "
                "Off: the view follows the camera's heading (smoothed)", false, gStab);
        dbl("smooth", "Smoothing", "How smoothly the view follows the camera when Direction Lock is off, seconds", 0.3, 0, 10, 0, 3, gStab);

        {
            ChoiceParamDescriptor* p = d.defineChoiceParam("quality");
            p->setLabels("Quality", "Quality", "Quality");
            p->setHint("High uses 2x2 supersampling (sharper when zoomed out, 4x slower)");
            p->appendOption("Normal");
            p->appendOption("High");
            p->setDefault(0);
            p->setAnimates(false);
            p->setParent(*gAdv);
            page->addChild(*p);
        }
        {
            IntParamDescriptor* p = d.defineIntParam("frameOffset");
            p->setLabels("Frame Offset", "Frame Offset", "Frame Offset");
            p->setHint("Shift which source frame is used, if the picture is out of sync with the timeline");
            p->setDefault(0);
            p->setRange(-100000, 100000);
            p->setDisplayRange(-30, 30);
            p->setAnimates(false);
            p->setParent(*gAdv);
            page->addChild(*p);
        }
        {
            StringParamDescriptor* p = d.defineStringParam("sourceOverride");
            p->setLabels("Source File", "Source File", "Source File");
            p->setHint("Optional: path to the .360 file, if it cannot be found automatically");
            p->setStringType(eStringTypeFilePath);
            p->setFilePathExists(true);
            p->setDefault("");
            p->setAnimates(false);
            p->setParent(*gAdv);
            page->addChild(*p);
        }
    }

    OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override
    {
        return new Max2ReframePlugin(handle);
    }
};

void OFX::Plugin::getPluginIDs(PluginFactoryArray& ids)
{
    static Max2ReframeFactory factory;
    ids.push_back(&factory);
}
