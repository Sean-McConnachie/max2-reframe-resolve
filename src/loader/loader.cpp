// Tiny OFX loader installed once into Resolve's standard plugin folder (needs admin).
// It forwards to the real plugin in %LOCALAPPDATA%\Max2Reframe\core\Max2ReframeCore.dll, so updating the plugin
// never needs admin rights. (Resolve neither follows a junction into the plugin folder nor reliably sees
// OFX_PLUGIN_PATH, so this is the dependable way to load from a user-writable location.)
#include <windows.h>
#include <shlobj.h>

#include <string>

#include "ofxCore.h"

// install.ps1 compares this marker (not the file bytes) to decide whether the installed loader needs replacing.
// Bump the number only when the loader's behaviour changes. Loaders without a marker count as version 1.
extern "C" __declspec(dllexport) const char Max2ReframeLoaderVersion[] = "MAX2REFRAME_LOADER_VERSION=1";

namespace {

HMODULE core()
{
    static HMODULE h = [] {
        PWSTR base = nullptr;
        HMODULE m = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)))
        {
            std::wstring path = std::wstring(base) + L"\\Max2Reframe\\core\\Max2ReframeCore.dll";
            m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            CoTaskMemFree(base);
        }
        return m;
    }();
    return h;
}

} // namespace

OfxExport int OfxGetNumberOfPlugins(void)
{
    HMODULE h = core();
    auto f = h ? reinterpret_cast<int (*)()>(GetProcAddress(h, "OfxGetNumberOfPlugins")) : nullptr;
    return f ? f() : 0;
}

OfxExport OfxPlugin* OfxGetPlugin(int nth)
{
    HMODULE h = core();
    auto f = h ? reinterpret_cast<OfxPlugin* (*)(int)>(GetProcAddress(h, "OfxGetPlugin")) : nullptr;
    return f ? f(nth) : nullptr;
}
