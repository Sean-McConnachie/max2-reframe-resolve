#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
std::mutex g_LogMutex;

std::wstring logPath()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(buf) + L"\\Max2Reframe" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\max2reframe.log";
}
} // namespace

void logf(const char* fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_LogMutex);
    static std::wstring path = logPath();
    FILE* f = _wfopen(path.c_str(), L"a");
    if (!f) return;
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    fclose(f);
}
