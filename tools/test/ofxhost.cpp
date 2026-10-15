// Minimal CPU OpenFX host for tests: loads one plugin and renders one frame from a raw float RGBA image.
// test_convert.py uses it to render with the GoPro Reframe plugin, as the reference for the converted settings.
//   ofxhost plugin.ofx pluginId in.raw inW inH out.raw [--project W H] [--time t] [--set name=value]... [--list]
//           [--page Fusion|Edit] [-v]
// Raw images: float32 RGBA, rows bottom-up (OFX convention). out.raw gets the output's region of definition; its
// size is printed.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxProperty.h"
#include "ofxTimeLine.h"
#include "ofxInteract.h"
#include "ofxGPURender.h"

using Val = std::variant<void*, std::string, double, int>;

struct PropSet
{
    std::map<std::string, std::vector<Val>> m;
    void set(const char* k, std::vector<Val> v) { m[k] = std::move(v); }
};

static bool g_Verbose = false;
static std::string g_Page = "Fusion";
static void miss(const char* what, const char* k)
{
    if (g_Verbose) fprintf(stderr, "  [host] %s missing: %s\n", what, k);
}

#define PS(h) (reinterpret_cast<PropSet*>(h))

static OfxStatus setVal(OfxPropertySetHandle h, const char* k, int i, Val v)
{
    auto& vec = PS(h)->m[k];
    if ((int)vec.size() <= i) vec.resize(i + 1, v);
    vec[i] = v;
    return kOfxStatOK;
}
static Val* getVal(OfxPropertySetHandle h, const char* k, int i)
{
    auto it = PS(h)->m.find(k);
    if (it == PS(h)->m.end() || i >= (int)it->second.size()) return nullptr;
    return &it->second[i];
}
static OfxStatus pSetPointer(OfxPropertySetHandle h, const char* k, int i, void* v) { return setVal(h, k, i, v); }
static OfxStatus pSetString(OfxPropertySetHandle h, const char* k, int i, const char* v) { return setVal(h, k, i, std::string(v ? v : "")); }
static OfxStatus pSetDouble(OfxPropertySetHandle h, const char* k, int i, double v) { return setVal(h, k, i, v); }
static OfxStatus pSetInt(OfxPropertySetHandle h, const char* k, int i, int v) { return setVal(h, k, i, v); }
static OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* k, int n, void* const* v) { for (int i = 0; i < n; ++i) setVal(h, k, i, v[i]); PS(h)->m[k].resize(n); return kOfxStatOK; }
static OfxStatus pSetStringN(OfxPropertySetHandle h, const char* k, int n, const char* const* v) { for (int i = 0; i < n; ++i) setVal(h, k, i, std::string(v[i])); PS(h)->m[k].resize(n); return kOfxStatOK; }
static OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* k, int n, const double* v) { for (int i = 0; i < n; ++i) setVal(h, k, i, v[i]); PS(h)->m[k].resize(n); return kOfxStatOK; }
static OfxStatus pSetIntN(OfxPropertySetHandle h, const char* k, int n, const int* v) { for (int i = 0; i < n; ++i) setVal(h, k, i, v[i]); PS(h)->m[k].resize(n); return kOfxStatOK; }
static OfxStatus pGetPointer(OfxPropertySetHandle h, const char* k, int i, void** v)
{
    Val* x = getVal(h, k, i);
    if (!x || !std::holds_alternative<void*>(*x)) { miss("ptr", k); *v = nullptr; return kOfxStatErrUnknown; }
    *v = std::get<void*>(*x);
    return kOfxStatOK;
}
static OfxStatus pGetString(OfxPropertySetHandle h, const char* k, int i, char** v)
{
    Val* x = getVal(h, k, i);
    if (!x || !std::holds_alternative<std::string>(*x)) { miss("str", k); static char e[1] = ""; *v = e; return kOfxStatErrUnknown; }
    *v = const_cast<char*>(std::get<std::string>(*x).c_str());
    return kOfxStatOK;
}
static OfxStatus pGetDouble(OfxPropertySetHandle h, const char* k, int i, double* v)
{
    Val* x = getVal(h, k, i);
    if (x && std::holds_alternative<double>(*x)) { *v = std::get<double>(*x); return kOfxStatOK; }
    if (x && std::holds_alternative<int>(*x)) { *v = std::get<int>(*x); return kOfxStatOK; }
    miss("dbl", k);
    *v = 0;
    return kOfxStatErrUnknown;
}
static OfxStatus pGetInt(OfxPropertySetHandle h, const char* k, int i, int* v)
{
    Val* x = getVal(h, k, i);
    if (x && std::holds_alternative<int>(*x)) { *v = std::get<int>(*x); return kOfxStatOK; }
    if (x && std::holds_alternative<double>(*x)) { *v = (int)std::get<double>(*x); return kOfxStatOK; }
    miss("int", k);
    *v = 0;
    return kOfxStatErrUnknown;
}
static OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* k, int n, void** v) { for (int i = 0; i < n; ++i) if (pGetPointer(h, k, i, v + i)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetStringN(OfxPropertySetHandle h, const char* k, int n, char** v) { for (int i = 0; i < n; ++i) if (pGetString(h, k, i, v + i)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* k, int n, double* v) { for (int i = 0; i < n; ++i) if (pGetDouble(h, k, i, v + i)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pGetIntN(OfxPropertySetHandle h, const char* k, int n, int* v) { for (int i = 0; i < n; ++i) if (pGetInt(h, k, i, v + i)) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus pReset(OfxPropertySetHandle, const char*) { return kOfxStatOK; }
static OfxStatus pGetDimension(OfxPropertySetHandle h, const char* k, int* n)
{
    auto it = PS(h)->m.find(k);
    if (it == PS(h)->m.end()) { miss("dim", k); *n = 0; return kOfxStatErrUnknown; }
    *n = (int)it->second.size();
    return kOfxStatOK;
}
static OfxPropertySuiteV1 g_PropSuite = {pSetPointer, pSetString, pSetDouble, pSetInt, pSetPointerN, pSetStringN, pSetDoubleN, pSetIntN,
                                         pGetPointer, pGetString, pGetDouble, pGetInt, pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN,
                                         pReset, pGetDimension};

// ---------------- params
struct Param
{
    std::string name, type;
    PropSet props;
    double d[4] = {0, 0, 0, 0};
    int i[3] = {0, 0, 0};
    std::string s;
};
struct ParamSet
{
    PropSet props;
    std::vector<std::unique_ptr<Param>> list;
    Param* find(const char* n)
    {
        for (auto& p : list) if (p->name == n) return p.get();
        return nullptr;
    }
};
static int dims(const std::string& t)
{
    if (t == kOfxParamTypeDouble2D || t == kOfxParamTypeInteger2D) return 2;
    if (t == kOfxParamTypeDouble3D || t == kOfxParamTypeInteger3D || t == kOfxParamTypeRGB) return 3;
    if (t == kOfxParamTypeRGBA) return 4;
    return 1;
}
static bool isDouble(const std::string& t)
{
    return t == kOfxParamTypeDouble || t == kOfxParamTypeDouble2D || t == kOfxParamTypeDouble3D || t == kOfxParamTypeRGB || t == kOfxParamTypeRGBA;
}
static bool isInt(const std::string& t)
{
    return t == kOfxParamTypeInteger || t == kOfxParamTypeInteger2D || t == kOfxParamTypeInteger3D || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice;
}
static bool isString(const std::string& t) { return t == kOfxParamTypeString || t == kOfxParamTypeCustom; }

static OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props)
{
    auto* set = reinterpret_cast<ParamSet*>(ps);
    auto p = std::make_unique<Param>();
    p->name = name;
    p->type = type;
    p->props.set(kOfxParamPropType, {std::string(type)});
    p->props.set(kOfxPropName, {std::string(name)});
    p->props.set(kOfxParamPropEnabled, {1});
    p->props.set(kOfxParamPropSecret, {0});
    p->props.set(kOfxParamPropAnimates, {1});
    p->props.set(kOfxParamPropIsAnimating, {0});
    p->props.set(kOfxParamPropIsAutoKeying, {0});
    p->props.set(kOfxParamPropPersistant, {1});
    p->props.set(kOfxParamPropEvaluateOnChange, {1});
    p->props.set(kOfxPropLabel, {std::string(name)});
    p->props.set(kOfxParamPropHint, {std::string("")});
    p->props.set(kOfxParamPropScriptName, {std::string(name)});
    p->props.set(kOfxParamPropParent, {std::string("")});
    p->props.set(kOfxParamPropDataPtr, {(void*)nullptr});
    if (props) *props = reinterpret_cast<OfxPropertySetHandle>(&p->props);
    set->list.push_back(std::move(p));
    return kOfxStatOK;
}
static OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props)
{
    Param* p = reinterpret_cast<ParamSet*>(ps)->find(name);
    if (!p)
    {
        // GoPro Reframe asks for "<group>_group" and "<group>_group_end"; give it empty groups.
        if (g_Verbose) fprintf(stderr, "  [host] unknown param %s (made a group)\n", name);
        OfxPropertySetHandle tmp;
        paramDefine(ps, kOfxParamTypeGroup, name, &tmp);
        p = reinterpret_cast<ParamSet*>(ps)->find(name);
    }
    *h = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = reinterpret_cast<OfxPropertySetHandle>(&p->props);
    return kOfxStatOK;
}
static OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle* props)
{
    *props = reinterpret_cast<OfxPropertySetHandle>(&reinterpret_cast<ParamSet*>(ps)->props);
    return kOfxStatOK;
}
static OfxStatus paramGetPropertySet(OfxParamHandle h, OfxPropertySetHandle* props)
{
    *props = reinterpret_cast<OfxPropertySetHandle>(&reinterpret_cast<Param*>(h)->props);
    return kOfxStatOK;
}
static OfxStatus getValueV(Param* p, va_list ap)
{
    int n = dims(p->type);
    if (isDouble(p->type)) for (int k = 0; k < n; ++k) *va_arg(ap, double*) = p->d[k];
    else if (isInt(p->type)) for (int k = 0; k < n; ++k) *va_arg(ap, int*) = p->i[k];
    else if (isString(p->type)) *va_arg(ap, const char**) = p->s.c_str();
    return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle h, ...)
{
    va_list ap;
    va_start(ap, h);
    OfxStatus s = getValueV(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
static OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime t, ...)
{
    va_list ap;
    va_start(ap, t);
    OfxStatus s = getValueV(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
static OfxStatus paramGetDerivative(OfxParamHandle h, OfxTime t, ...)
{
    Param* p = reinterpret_cast<Param*>(h);
    va_list ap;
    va_start(ap, t);
    for (int k = 0; k < dims(p->type); ++k) *va_arg(ap, double*) = 0;
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus paramGetIntegral(OfxParamHandle h, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
static OfxStatus setValueV(Param* p, va_list ap)
{
    int n = dims(p->type);
    if (isDouble(p->type)) for (int k = 0; k < n; ++k) p->d[k] = va_arg(ap, double);
    else if (isInt(p->type)) for (int k = 0; k < n; ++k) p->i[k] = va_arg(ap, int);
    else if (isString(p->type)) p->s = va_arg(ap, const char*);
    return kOfxStatOK;
}
static OfxStatus paramSetValue(OfxParamHandle h, ...)
{
    va_list ap;
    va_start(ap, h);
    OfxStatus s = setValueV(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
static OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime t, ...)
{
    va_list ap;
    va_start(ap, t);
    OfxStatus s = setValueV(reinterpret_cast<Param*>(h), ap);
    va_end(ap);
    return s;
}
static OfxStatus paramGetNumKeys(OfxParamHandle, unsigned int* n) { *n = 0; return kOfxStatOK; }
static OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
static OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
static OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
static OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
static OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
static OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
static OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }
static OfxParameterSuiteV1 g_ParamSuite = {paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
                                           paramGetValueAtTime, paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime,
                                           paramGetNumKeys, paramGetKeyTime, paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys, paramCopy,
                                           paramEditBegin, paramEditEnd};

// ---------------- effect, clips, images
struct Image
{
    PropSet props;
    std::vector<float> px;
};
struct Clip
{
    std::string name;
    PropSet props;
    int w = 0, h = 0;  // RoD
    int x0 = 0, y0 = 0;
    std::vector<float>* data = nullptr;
    std::vector<std::unique_ptr<Image>> images;
};
struct Effect
{
    PropSet props;
    ParamSet params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
};

static OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* p)
{
    *p = reinterpret_cast<OfxPropertySetHandle>(&reinterpret_cast<Effect*>(e)->props);
    return kOfxStatOK;
}
static OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p)
{
    *p = reinterpret_cast<OfxParamSetHandle>(&reinterpret_cast<Effect*>(e)->params);
    return kOfxStatOK;
}
static OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* props)
{
    auto c = std::make_unique<Clip>();
    c->name = name;
    c->props.set(kOfxPropName, {std::string(name)});
    if (props) *props = reinterpret_cast<OfxPropertySetHandle>(&c->props);
    reinterpret_cast<Effect*>(e)->clips[name] = std::move(c);
    return kOfxStatOK;
}
static OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props)
{
    auto& m = reinterpret_cast<Effect*>(e)->clips;
    auto it = m.find(name);
    if (it == m.end()) return kOfxStatErrBadHandle;
    *clip = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (props) *props = reinterpret_cast<OfxPropertySetHandle>(&it->second->props);
    return kOfxStatOK;
}
static OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* props)
{
    *props = reinterpret_cast<OfxPropertySetHandle>(&reinterpret_cast<Clip*>(c)->props);
    return kOfxStatOK;
}
static void imageProps(PropSet& p, void* data, int w, int h)
{
    p.set(kOfxImagePropData, {data});
    p.set(kOfxImagePropBounds, {0, 0, w, h});
    p.set(kOfxImagePropRegionOfDefinition, {0, 0, w, h});
    p.set(kOfxImagePropRowBytes, {w * 16});
    p.set(kOfxImageEffectPropPixelDepth, {std::string(kOfxBitDepthFloat)});
    p.set(kOfxImageEffectPropComponents, {std::string(kOfxImageComponentRGBA)});
    p.set(kOfxImageEffectPropPreMultiplication, {std::string(kOfxImagePreMultiplied)});
    p.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    p.set(kOfxImagePropPixelAspectRatio, {1.0});
    p.set(kOfxImagePropField, {std::string(kOfxImageFieldNone)});
    p.set(kOfxImagePropUniqueIdentifier, {std::string("img") + std::to_string((uintptr_t)data)});
}
static OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime, const OfxRectD*, OfxPropertySetHandle* out)
{
    Clip* c = reinterpret_cast<Clip*>(ch);
    if (!c->data) return kOfxStatFailed;
    auto im = std::make_unique<Image>();
    imageProps(im->props, c->data->data(), c->w, c->h);
    im->props.set(kOfxImagePropBounds, {c->x0, c->y0, c->x0 + c->w, c->y0 + c->h});
    im->props.set(kOfxImagePropRegionOfDefinition, {c->x0, c->y0, c->x0 + c->w, c->y0 + c->h});
    *out = reinterpret_cast<OfxPropertySetHandle>(&im->props);
    c->images.push_back(std::move(im));
    return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle) { return kOfxStatOK; }
static OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle ch, OfxTime, OfxRectD* r)
{
    Clip* c = reinterpret_cast<Clip*>(ch);
    *r = {double(c->x0), double(c->y0), double(c->x0 + c->w), double(c->y0 + c->h)};
    return kOfxStatOK;
}
static int abortFn(OfxImageEffectHandle) { return 0; }
static OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h)
{
    *h = reinterpret_cast<OfxImageMemoryHandle>(malloc(n));
    return *h ? kOfxStatOK : kOfxStatErrMemory;
}
static OfxStatus imageMemoryFree(OfxImageMemoryHandle h) { free(h); return kOfxStatOK; }
static OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void** p) { *p = h; return kOfxStatOK; }
static OfxStatus imageMemoryUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }
static OfxImageEffectSuiteV1 g_EffectSuite = {getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage,
                                              clipReleaseImage, clipGetRegionOfDefinition, abortFn, imageMemoryAlloc, imageMemoryFree,
                                              imageMemoryLock, imageMemoryUnlock};

static OfxStatus memAlloc(void*, size_t n, void** p) { *p = malloc(n); return *p ? kOfxStatOK : kOfxStatErrMemory; }
static OfxStatus memFree(void* p) { free(p); return kOfxStatOK; }
static OfxMemorySuiteV1 g_MemSuite = {memAlloc, memFree};

static OfxStatus mtRun(OfxThreadFunctionV1 f, unsigned int n, void* arg)
{
    for (unsigned i = 0; i < n; ++i) f(i, n, arg);
    return kOfxStatOK;
}
static OfxStatus mtNum(unsigned int* n) { *n = 1; return kOfxStatOK; }
static OfxStatus mtIndex(unsigned int* i) { *i = 0; return kOfxStatOK; }
static int mtIsSpawned() { return 0; }
static OfxStatus mxCreate(OfxMutexHandle* m, int) { *m = reinterpret_cast<OfxMutexHandle>(new std::recursive_mutex); return kOfxStatOK; }
static OfxStatus mxDestroy(const OfxMutexHandle m) { delete reinterpret_cast<std::recursive_mutex*>(m); return kOfxStatOK; }
static OfxStatus mxLock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex*>(m)->lock(); return kOfxStatOK; }
static OfxStatus mxUnlock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex*>(m)->unlock(); return kOfxStatOK; }
static OfxStatus mxTry(const OfxMutexHandle m) { return reinterpret_cast<std::recursive_mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
static OfxMultiThreadSuiteV1 g_MtSuite = {mtRun, mtNum, mtIndex, mtIsSpawned, mxCreate, mxDestroy, mxLock, mxUnlock, mxTry};

static OfxStatus msg(void*, const char* type, const char* id, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [plugin %s %s] ", type ? type : "", id ? id : "");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus msgClear(void*) { return kOfxStatOK; }
static OfxMessageSuiteV1 g_MsgSuite1 = {msg};
static OfxMessageSuiteV2 g_MsgSuite2 = {msg, msg, msgClear};

static double g_Time = 0;
static OfxStatus tlGetTime(void*, double* t) { *t = g_Time; return kOfxStatOK; }
static OfxStatus tlGoto(void*, double) { return kOfxStatOK; }
static OfxStatus tlBounds(void*, double* a, double* b) { *a = 0; *b = 100000; return kOfxStatOK; }
static OfxTimeLineSuiteV1 g_TlSuite = {tlGetTime, tlGoto, tlBounds};

static OfxStatus prStart(void*, const char*) { return kOfxStatOK; }
static OfxStatus prUpdate(void*, double) { return kOfxStatOK; }
static OfxStatus prEnd(void*) { return kOfxStatOK; }
static OfxProgressSuiteV1 g_PrSuite = {prStart, prUpdate, prEnd};

static OfxStatus iaSwap(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus iaRedraw(OfxInteractHandle) { return kOfxStatOK; }
static OfxStatus iaProps(OfxInteractHandle, OfxPropertySetHandle* p) { static PropSet ps; *p = reinterpret_cast<OfxPropertySetHandle>(&ps); return kOfxStatOK; }
static OfxInteractSuiteV1 g_IaSuite = {iaSwap, iaRedraw, iaProps};
static PropSet g_HostProps;
static const void* fetchSuite(OfxPropertySetHandle, const char* name, int version)
{
    std::string n = name;
    if (n == kOfxPropertySuite) return &g_PropSuite;
    if (n == kOfxImageEffectSuite) return &g_EffectSuite;
    if (n == kOfxParameterSuite) return &g_ParamSuite;
    if (n == kOfxMemorySuite) return &g_MemSuite;
    if (n == kOfxMultiThreadSuite) return &g_MtSuite;
    if (n == kOfxMessageSuite) return version >= 2 ? (const void*)&g_MsgSuite2 : (const void*)&g_MsgSuite1;
    if (n == kOfxTimeLineSuite) return &g_TlSuite;
    if (n == kOfxProgressSuite) return &g_PrSuite;
    if (n == kOfxInteractSuite) return &g_IaSuite;
    if (g_Verbose) fprintf(stderr, "  [host] no suite %s v%d\n", name, version);
    return nullptr;
}
static OfxHost g_Host = {reinterpret_cast<OfxPropertySetHandle>(&g_HostProps), fetchSuite};

static OfxPlugin* g_Plugin = nullptr;
static OfxStatus act(const char* a, void* h, PropSet* in = nullptr, PropSet* out = nullptr)
{
    OfxStatus s = g_Plugin->mainEntry(a, h, reinterpret_cast<OfxPropertySetHandle>(in), reinterpret_cast<OfxPropertySetHandle>(out));
    if (g_Verbose || (s != kOfxStatOK && s != kOfxStatReplyDefault)) fprintf(stderr, "action %s -> %d\n", a, s);
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 7) { fprintf(stderr, "usage: ofxhost plugin.ofx id in.raw inW inH out.raw [opts]\n"); return 2; }
    const char* dll = argv[1];
    std::string id = argv[2];
    const char* inPath = argv[3];
    int inW = atoi(argv[4]), inH = atoi(argv[5]);
    const char* outPath = argv[6];
    int projW = 3840, projH = 2160;
    std::vector<std::pair<std::string, std::string>> sets;
    bool list = false;
    for (int i = 7; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--project" && i + 2 < argc) { projW = atoi(argv[++i]); projH = atoi(argv[++i]); }
        else if (a == "--time" && i + 1 < argc) g_Time = atof(argv[++i]);
        else if (a == "--set" && i + 1 < argc)
        {
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            sets.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
        else if (a == "--list") list = true;
        else if (a == "-v") g_Verbose = true;
        else if (a == "--page" && i + 1 < argc) g_Page = argv[++i];
    }

    g_HostProps.set(kOfxPropName, {std::string("DaVinciResolveLite")});
    g_HostProps.set(kOfxPropLabel, {std::string("DaVinci Resolve")});
    g_HostProps.set(kOfxPropVersion, {21, 1, 0});
    g_HostProps.set(kOfxPropVersionLabel, {std::string("21.1")});
    g_HostProps.set(kOfxPropAPIVersion, {1, 4});
    g_HostProps.set(kOfxImageEffectHostPropIsBackground, {0});
    g_HostProps.set(kOfxImageEffectPropSupportsOverlays, {0});
    g_HostProps.set(kOfxImageEffectPropSupportsMultiResolution, {1});
    g_HostProps.set(kOfxImageEffectPropSupportsTiles, {0});
    g_HostProps.set(kOfxImageEffectPropTemporalClipAccess, {0});
    g_HostProps.set(kOfxImageEffectPropSupportedComponents, {std::string(kOfxImageComponentRGBA)});
    g_HostProps.set(kOfxImageEffectPropSupportedContexts, {std::string(kOfxImageEffectContextFilter), std::string(kOfxImageEffectContextGeneral)});
    g_HostProps.set(kOfxImageEffectPropSupportedPixelDepths, {std::string(kOfxBitDepthFloat)});
    g_HostProps.set(kOfxImageEffectPropSupportsMultipleClipDepths, {0});
    g_HostProps.set(kOfxImageEffectPropSupportsMultipleClipPARs, {0});
    g_HostProps.set(kOfxImageEffectPropSetableFrameRate, {0});
    g_HostProps.set(kOfxImageEffectPropSetableFielding, {0});
    g_HostProps.set(kOfxParamHostPropSupportsCustomInteract, {0});
    g_HostProps.set(kOfxParamHostPropSupportsStringAnimation, {0});
    g_HostProps.set(kOfxParamHostPropSupportsChoiceAnimation, {0});
    g_HostProps.set(kOfxParamHostPropSupportsBooleanAnimation, {0});
    g_HostProps.set(kOfxParamHostPropSupportsCustomAnimation, {0});
    g_HostProps.set(kOfxParamHostPropMaxParameters, {-1});
    g_HostProps.set(kOfxParamHostPropMaxPages, {0});
    g_HostProps.set(kOfxParamHostPropPageRowColumnCount, {0, 0});
    g_HostProps.set(kOfxPropHostOSHandle, {(void*)nullptr});
    g_HostProps.set(kOfxImageEffectPropOpenCLRenderSupported, {std::string("false")});
    g_HostProps.set(kOfxImageEffectPropCudaRenderSupported, {std::string("false")});
    g_HostProps.set(kOfxImageEffectPropCudaStreamSupported, {std::string("false")});
    g_HostProps.set(kOfxImageEffectPropMetalRenderSupported, {std::string("false")});
    g_HostProps.set(kOfxImageEffectPropRenderQualityDraft, {0});
    g_HostProps.set(kOfxImageEffectInstancePropSequentialRender, {0});

    HMODULE mod = LoadLibraryA(dll);
    if (!mod) { fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto getN = (int (*)())GetProcAddress(mod, "OfxGetNumberOfPlugins");
    auto getP = (OfxPlugin * (*)(int)) GetProcAddress(mod, "OfxGetPlugin");
    if (auto setBundle = (OfxStatus(*)(const char*))GetProcAddress(mod, "OfxSetBundleDirectory")) (void)setBundle;
    int n = getN();
    for (int i = 0; i < n; ++i)
    {
        OfxPlugin* p = getP(i);
        fprintf(stderr, "plugin %d: %s (%s) v%u.%u\n", i, p->pluginIdentifier, p->pluginApi, p->pluginVersionMajor, p->pluginVersionMinor);
        if (id == p->pluginIdentifier) g_Plugin = p;
    }
    if (!g_Plugin) { fprintf(stderr, "plugin %s not found\n", id.c_str()); return 1; }
    g_Plugin->setHost(&g_Host);
    act(kOfxActionLoad, nullptr);

    Effect desc;
    desc.props.set(kOfxPropType, {std::string(kOfxTypeImageEffect)});
    act(kOfxActionDescribe, &desc);
    Effect ctxDesc;
    ctxDesc.props = desc.props;
    PropSet ctxIn;
    ctxIn.set(kOfxImageEffectPropContext, {std::string(kOfxImageEffectContextFilter)});
    act(kOfxImageEffectActionDescribeInContext, &ctxDesc, &ctxIn);

    Effect inst;
    inst.props = ctxDesc.props;
    inst.props.set(kOfxPropType, {std::string(kOfxTypeImageEffectInstance)});
    inst.props.set(kOfxImageEffectPropContext, {std::string(kOfxImageEffectContextFilter)});
    inst.props.set(kOfxPropIsInteractive, {0});
    inst.props.set(kOfxImageEffectPropProjectSize, {double(projW), double(projH)});
    inst.props.set(kOfxImageEffectPropProjectExtent, {double(projW), double(projH)});
    inst.props.set(kOfxImageEffectPropProjectOffset, {0.0, 0.0});
    inst.props.set(kOfxImageEffectPropProjectPixelAspectRatio, {1.0});
    inst.props.set(kOfxImageEffectInstancePropEffectDuration, {10000.0});
    inst.props.set(kOfxImageEffectInstancePropSequentialRender, {0});
    inst.props.set(kOfxImageEffectPropFrameRate, {29.97});
    inst.props.set(kOfxPropInstanceData, {(void*)nullptr});
    inst.props.set(kOfxImageEffectPropSupportsTiles, {0});
    inst.props.set("OfxImageEffectPropResolvePage", {std::string(g_Page)});
    inst.props.set("OfxImageEffectPropSrcFilePath", {std::string("")});
    for (auto& p : ctxDesc.params.list)
    {
        auto q = std::make_unique<Param>(*p);
        // defaults
        Val* dv = getVal(reinterpret_cast<OfxPropertySetHandle>(&q->props), kOfxParamPropDefault, 0);
        for (int k = 0; k < dims(q->type); ++k)
        {
            Val* v = getVal(reinterpret_cast<OfxPropertySetHandle>(&q->props), kOfxParamPropDefault, k);
            if (!v) continue;
            if (isDouble(q->type)) q->d[k] = std::holds_alternative<double>(*v) ? std::get<double>(*v) : std::get<int>(*v);
            else if (isInt(q->type)) q->i[k] = std::holds_alternative<int>(*v) ? std::get<int>(*v) : (int)std::get<double>(*v);
        }
        if (isString(q->type) && dv && std::holds_alternative<std::string>(*dv)) q->s = std::get<std::string>(*dv);
        inst.params.list.push_back(std::move(q));
    }
    for (auto& c : ctxDesc.clips)
    {
        auto d = std::make_unique<Clip>();
        d->name = c.first;
        d->props = c.second->props;
        inst.clips[c.first] = std::move(d);
    }
    std::vector<float> src(size_t(inW) * inH * 4);
    {
        FILE* f = fopen(inPath, "rb");
        if (!f) { fprintf(stderr, "cannot read %s\n", inPath); return 1; }
        fread(src.data(), 4, src.size(), f);
        fclose(f);
    }
    for (auto& c : inst.clips)
    {
        Clip& cl = *c.second;
        bool isSrc = c.first == kOfxImageEffectSimpleSourceClipName;
        cl.w = isSrc ? inW : projW;
        cl.h = isSrc ? inH : projH;
        cl.props.set(kOfxImageClipPropConnected, {1});
        cl.props.set(kOfxImageEffectPropPixelDepth, {std::string(kOfxBitDepthFloat)});
        cl.props.set(kOfxImageEffectPropComponents, {std::string(kOfxImageComponentRGBA)});
        cl.props.set(kOfxImageClipPropUnmappedPixelDepth, {std::string(kOfxBitDepthFloat)});
        cl.props.set(kOfxImageClipPropUnmappedComponents, {std::string(kOfxImageComponentRGBA)});
        cl.props.set(kOfxImageEffectPropPreMultiplication, {std::string(kOfxImagePreMultiplied)});
        cl.props.set(kOfxImagePropPixelAspectRatio, {1.0});
        cl.props.set(kOfxImageEffectPropFrameRate, {29.97});
        cl.props.set(kOfxImageEffectPropFrameRange, {0.0, 10000.0});
        cl.props.set(kOfxImageEffectPropUnmappedFrameRange, {0.0, 10000.0});
        cl.props.set(kOfxImageEffectPropUnmappedFrameRate, {29.97});
        cl.props.set(kOfxImageClipPropFieldOrder, {std::string(kOfxImageFieldNone)});
        cl.props.set(kOfxImageClipPropContinuousSamples, {0});
        cl.props.set(kOfxImageClipPropIsMask, {0});
        cl.props.set(kOfxImageClipPropOptional, {0});
        cl.props.set("kOfxImageClipPropThumbnail", {0});
        if (isSrc) cl.data = &src;
    }
    act(kOfxActionCreateInstance, &inst);

    for (auto& kv : sets)
    {
        Param* p = inst.params.find(kv.first.c_str());
        if (!p) { fprintf(stderr, "no param %s\n", kv.first.c_str()); return 1; }
        if (isDouble(p->type)) p->d[0] = atof(kv.second.c_str());
        else if (isInt(p->type)) p->i[0] = atoi(kv.second.c_str());
        else p->s = kv.second;
        PropSet in;
        in.set(kOfxPropType, {std::string(kOfxTypeParameter)});
        in.set(kOfxPropName, {kv.first});
        in.set(kOfxPropChangeReason, {std::string(kOfxChangeUserEdited)});
        in.set(kOfxPropTime, {g_Time});
        in.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
        act(kOfxActionBeginInstanceChanged, &inst, &in);
        act(kOfxActionInstanceChanged, &inst, &in);
        act(kOfxActionEndInstanceChanged, &inst, &in);
    }
    if (list)
        for (auto& p : inst.params.list)
            fprintf(stderr, "param %-28s %-22s d=%g i=%d s=%s\n", p->name.c_str(), p->type.c_str(), p->d[0], p->i[0], p->s.c_str());

    PropSet rodIn, rodOut;
    rodIn.set(kOfxPropTime, {g_Time});
    rodIn.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    rodOut.set(kOfxImageEffectPropRegionOfDefinition, {0.0, 0.0, double(projW), double(projH)});
    act(kOfxImageEffectActionGetRegionOfDefinition, &inst, &rodIn, &rodOut);
    double rod[4];
    for (int k = 0; k < 4; ++k) pGetDouble(reinterpret_cast<OfxPropertySetHandle>(&rodOut), kOfxImageEffectPropRegionOfDefinition, k, rod + k);
    fprintf(stderr, "output RoD %g %g %g %g\n", rod[0], rod[1], rod[2], rod[3]);
    int ow = int(rod[2] - rod[0]), oh = int(rod[3] - rod[1]);
    std::vector<float> dst(size_t(ow) * oh * 4, 0.0f);
    Clip& out = *inst.clips[kOfxImageEffectOutputClipName];
    out.w = ow;
    out.h = oh;
    out.x0 = int(rod[0]);
    out.y0 = int(rod[1]);
    out.data = &dst;

    PropSet seq;
    seq.set(kOfxImageEffectPropFrameRange, {g_Time, g_Time});
    seq.set(kOfxImageEffectPropFrameStep, {1.0});
    seq.set(kOfxPropIsInteractive, {0});
    seq.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    seq.set(kOfxImageEffectPropSequentialRenderStatus, {0});
    seq.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
    act(kOfxImageEffectActionBeginSequenceRender, &inst, &seq);
    PropSet rin;
    rin.set(kOfxPropTime, {g_Time});
    rin.set(kOfxImageEffectPropFieldToRender, {std::string(kOfxImageFieldNone)});
    rin.set(kOfxImageEffectPropRenderWindow, {int(rod[0]), int(rod[1]), int(rod[2]), int(rod[3])});
    rin.set(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    rin.set(kOfxImageEffectPropSequentialRenderStatus, {0});
    rin.set(kOfxImageEffectPropInteractiveRenderStatus, {0});
    rin.set(kOfxImageEffectPropRenderQualityDraft, {0});
    rin.set(kOfxImageEffectPropOpenCLEnabled, {0});
    rin.set(kOfxImageEffectPropCudaEnabled, {0});
    rin.set(kOfxImageEffectPropMetalEnabled, {0});
    OfxStatus rs = act(kOfxImageEffectActionRender, &inst, &rin);
    act(kOfxImageEffectActionEndSequenceRender, &inst, &seq);
    FILE* f = fopen(outPath, "wb");
    fwrite(dst.data(), 4, dst.size(), f);
    fclose(f);
    printf("%d %d %d\n", ow, oh, rs);
    return rs == kOfxStatOK ? 0 : 1;
}
