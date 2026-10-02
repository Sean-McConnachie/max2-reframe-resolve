// OpenFX plugin: reframes GoPro Max/Max 2 .360 clips straight from the original file, with gyro stabilization.
//
// Resolve hands OFX effects images at timeline resolution, so instead of using the input image this plugin
// reads the clip's source path (kOfxImageEffectPropSrcFilePath) and decodes the .360 itself at full
// resolution. The input image is only used as a pass-through when the source is not a .360 file.
// Rendering runs on CUDA or OpenCL (whichever Resolve is using), with a CPU fallback.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#include <windows.h>

#include "ofxsImageEffect.h"

#include "engine.h"
#include "gpu.h"
#include "log.h"

#define kPluginName "Max2 Reframe"
#define kPluginGrouping "GoPro 360"
#define kPluginDescription \
    "Reframes GoPro Max / Max 2 .360 clips directly from the original file at full resolution, " \
    "with gyro stabilization, horizon lock and direction lock."
#define kPluginIdentifier "com.max2resolve.reframe"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 2

namespace {

std::atomic<int> g_RenderCount{0};

bool shouldLog()
{
    int n = g_RenderCount++;
    return n < 30 || n % 300 == 0;
}

double msSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

enum class Backend { Cpu, Cuda, OpenCL };

const char* backendName(Backend b) { return b == Backend::Cuda ? "CUDA" : b == Backend::OpenCL ? "OpenCL" : "CPU"; }

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
        m_MotionBlur = fetchBooleanParam("motionBlur");
        m_Shutter = fetchDoubleParam("shutter");
        m_MbSamples = fetchIntParam("mbSamples");
        m_Override = fetchStringParam("sourceOverride");
        m_SrcPath = readSrcPath();
        logf("instance created, source path \"%s\"", m_SrcPath.c_str());
    }

    void render(const OFX::RenderArguments& args) override
    {
        auto t0 = std::chrono::steady_clock::now();
        Backend backend = args.isEnabledCudaRender ? Backend::Cuda : args.isEnabledOpenCLRender ? Backend::OpenCL : Backend::Cpu;
        void* gpuQueue = backend == Backend::Cuda ? args.pCudaStream : args.pOpenCLCmdQ;

        std::unique_ptr<OFX::Image> dst(m_Dst->fetchImage(args.time));
        if (!dst || dst->getPixelDepth() != OFX::eBitDepthFloat || dst->getPixelComponents() != OFX::ePixelComponentRGBA)
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        const OfxRectI b = dst->getBounds();
        const int W = b.x2 - b.x1, H = b.y2 - b.y1;
        int rowBytes = dst->getRowBytes();

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
            if (shouldLog())
                logf("render t=%.2f: no usable .360 source (\"%s\": %s), passing input through", args.time, path.c_str(),
                     path.empty() ? "host gave no source path" : err.c_str());
            passThrough(args, dst.get(), backend, gpuQueue);
            return;
        }

        const auto& info = clip->info();
        double hostFrame = args.srcFrame >= 0 ? double(args.srcFrame) : args.time;
        int frame = resolveSourceFrame(info, hostFrame) + m_FrameOffset->getValueAtTime(args.time);
        frame = std::clamp(frame, 0, info.frames - 1);

        std::shared_ptr<const Nv12Frame> s0, s1;
        if (!clip->fetch(frame, s0, s1, &err))
        {
            logf("render t=%.2f frame %d: decode failed: %s", args.time, frame, err.c_str());
            passThrough(args, dst.get(), backend, gpuQueue);
            return;
        }
        double decodeMs = msSince(t0);

        // GPU buffers from Resolve are packed rows; fall back to that if the host reports no row bytes.
        int strideFloats = rowBytes > 0 ? rowBytes / int(sizeof(float)) : W * 4;
        MotionBlurSettings mb;
        mb.enabled = m_MotionBlur->getValueAtTime(args.time);
        mb.shutterAngle = m_Shutter->getValueAtTime(args.time);
        mb.maxSamples = m_MbSamples->getValueAtTime(args.time);
        double blurPx = 0;
        std::vector<RfParams> steps = buildRenderSteps(
            *clip, frame, [&](double dt) { return settings(args.time + dt); }, mb, *s0, W, H, strideFloats, &blurPx);
        const int n = int(steps.size());

        bool ok = true;
        if (backend == Backend::Cuda) ok = cudaRender(gpuQueue, steps.data(), n, s0, s1, static_cast<float*>(dst->getPixelData()), &err);
        else if (backend == Backend::OpenCL) ok = openclRender(gpuQueue, steps.data(), n, s0, s1, dst->getPixelData(), &err);
        else Reprojector(*s0, *s1, steps).renderAll(static_cast<float*>(dst->getPixelAddress(b.x1, b.y1)), rowBytes / int(sizeof(float)));

        if (!ok)
        {
            // GPU path failed: render on the CPU and upload the result so the timeline still shows a picture.
            logf("%s render failed (%s), using CPU fallback", backendName(backend), err.c_str());
            std::vector<float> host(size_t(W) * H * 4);
            for (RfParams& p : steps) p.outStride = W * 4;
            Reprojector(*s0, *s1, steps).renderAll(host.data(), W * 4);
            if (backend == Backend::Cuda) cudaCopy(gpuQueue, dst->getPixelData(), host.data(), host.size() * sizeof(float), true, &err);
            else openclCopy(gpuQueue, dst->getPixelData(), host.data(), host.size() * sizeof(float), true, &err);
        }

        if (shouldLog())
            logf("render %s t=%.2f srcFrame=%d -> frame %d (got %d/%d), out %dx%d rowBytes %d scale %.3f, %d sample(s) "
                 "(blur %.1f px): decode %.1f ms, total %.1f ms",
                 backendName(backend), args.time, args.srcFrame, frame, s0->index, s1->index, W, H, rowBytes,
                 args.renderScale.x, n, blurPx, decodeMs, msSince(t0));
    }

    bool isIdentity(const OFX::IsIdentityArguments&, OFX::Clip*&, double&) override { return false; }

private:
    RenderSettings settings(double t)
    {
        RenderSettings rs;
        rs.stab.stabilize = m_Stabilize->getValueAtTime(t);
        rs.stab.horizon = m_Horizon->getValueAtTime(t);
        rs.stab.directionLock = m_DirLock->getValueAtTime(t);
        rs.stab.smoothSeconds = m_Smooth->getValueAtTime(t);
        rs.pan = m_Pan->getValueAtTime(t);
        rs.tilt = m_Tilt->getValueAtTime(t);
        rs.roll = m_Roll->getValueAtTime(t);
        rs.fovDeg = m_Fov->getValueAtTime(t);
        rs.curvature = m_Curv->getValueAtTime(t);
        int proj = 0, quality = 0;
        m_Proj->getValueAtTime(t, proj);
        m_Quality->getValueAtTime(t, quality);
        rs.proj = proj == 1 ? Projection::Equirectangular : Projection::Lens;
        rs.supersample = quality == 1 ? 2 : 1;
        return rs;
    }

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

    void passThrough(const OFX::RenderArguments& args, OFX::Image* dst, Backend backend, void* queue)
    {
        std::unique_ptr<OFX::Image> src(m_Src && m_Src->isConnected() ? m_Src->fetchImage(args.time) : nullptr);
        const OfxRectI b = dst->getBounds();
        if (backend != Backend::Cpu)
        {
            size_t bytes = size_t(b.x2 - b.x1) * (b.y2 - b.y1) * 4 * sizeof(float);
            std::string err;
            if (src && src->getBounds().x2 == b.x2 && src->getBounds().y2 == b.y2)
            {
                bool ok = backend == Backend::Cuda ? cudaCopy(queue, dst->getPixelData(), src->getPixelData(), bytes, false, &err)
                                                   : openclCopy(queue, dst->getPixelData(), src->getPixelData(), bytes, false, &err);
                if (!ok) logf("pass-through copy failed: %s", err.c_str());
            }
            return;
        }
        for (int y = b.y1; y < b.y2; ++y)
        {
            float* d = static_cast<float*>(dst->getPixelAddress(b.x1, y));
            for (int x = b.x1; x < b.x2; ++x, d += 4)
            {
                const float* s = src ? static_cast<const float*>(src->getPixelAddress(x, y)) : nullptr;
                if (s) std::memcpy(d, s, 4 * sizeof(float));
                else d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }

    OFX::Clip* m_Dst;
    OFX::Clip* m_Src;
    OFX::BooleanParam *m_Stabilize, *m_Horizon, *m_DirLock;
    OFX::DoubleParam *m_Smooth, *m_Pan, *m_Tilt, *m_Roll, *m_Fov, *m_Curv;
    OFX::ChoiceParam *m_Proj, *m_Quality;
    OFX::IntParam* m_FrameOffset;
    OFX::BooleanParam* m_MotionBlur;
    OFX::DoubleParam* m_Shutter;
    OFX::IntParam* m_MbSamples;
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
        // No logging from DllMain: it runs under the loader lock, where shell calls (the log path) can fail.
        logf("plugin loaded, describe (v%d.%d, pid %lu)", kPluginVersionMajor, kPluginVersionMinor, GetCurrentProcessId());
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
        d.setSupportsCudaRender(true);
        d.setSupportsCudaStream(true);
        d.setSupportsOpenCLRender(true);
        logf("describe done");
    }

    void describeInContext(OFX::ImageEffectDescriptor& d, OFX::ContextEnum ctx) override
    {
        logf("describeInContext %d", int(ctx));
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
        GroupParamDescriptor* gMb = d.defineGroupParam("mbGroup");
        gMb->setLabels("Motion Blur", "Motion Blur", "Motion Blur");
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

        dbl("pan", "Pan", "Look left (-) / right (+), degrees. Keyframe 0 to 360 for a full spin.", 0, -36000, 36000, -360, 360, gView);
        dbl("tilt", "Tilt", "Look down (-) / up (+), degrees", 0, -360, 360, -180, 180, gView);
        dbl("roll", "Roll", "Rotate the view, degrees", 0, -36000, 36000, -180, 180, gView);
        dbl("fov", "Field of View", "Horizontal field of view, degrees (up to 360 with a fisheye lens curvature)", 100, 1, 360, 10, 360, gView);
        dbl("curv", "Lens Curvature",
            "0 = rectilinear (straight lines), 1 = stereographic (wide, tiny planet), 2 = fisheye (can show all 360 degrees)",
            0.4, 0, 2, 0, 2, gView);
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

        boolean("motionBlur", "Motion Blur",
                "Blur the picture along the virtual camera's movement (pans, tilts, zooms and the smoothed heading "
                "follow), like a real camera's shutter", false, gMb);
        dbl("shutter", "Shutter Angle", "Exposure as a fraction of the frame interval: 180 = half the frame (film look), "
            "360 = the whole frame", 180, 0, 360, 0, 360, gMb);
        {
            IntParamDescriptor* p = d.defineIntParam("mbSamples");
            p->setLabels("Max Samples", "Max Samples", "Max Samples");
            p->setHint("Upper limit on blur samples per pixel; the plugin uses about one per pixel of blur. "
                       "Lower is faster, higher is smoother for very fast moves.");
            p->setDefault(32);
            p->setRange(2, 128);
            p->setDisplayRange(2, 64);
            p->setAnimates(false);
            p->setParent(*gMb);
            page->addChild(*p);
        }

        {
            ChoiceParamDescriptor* p = d.defineChoiceParam("quality");
            p->setLabels("Quality", "Quality", "Quality");
            p->setHint("High uses 2x2 supersampling: smoother edges, especially with wide or fisheye views");
            p->appendOption("Normal");
            p->appendOption("High");
            p->setDefault(1);
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
    logf("getPluginIDs (pid %lu)", GetCurrentProcessId());
    static Max2ReframeFactory factory;
    ids.push_back(&factory);
}
