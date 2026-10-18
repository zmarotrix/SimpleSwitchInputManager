#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

static std::mutex g_logLock;
static FILE* g_logFile = nullptr;
static bool g_logConsole = false;

void LogInit(bool toConsole, bool toFile)
{
    g_logConsole = toConsole;
    if (!toFile) {
        return;
    }
    std::wstring path = ExeDir() + L"\\Switch2Pro.log";
    // Keep one previous log around; truncate the current one each start.
    std::wstring old = path + L".old";
    MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_logFile = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);

    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_logConsole) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

std::wstring ExePath()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf, n);
}

std::wstring ExeDir()
{
    std::wstring p = ExePath();
    return p.substr(0, p.find_last_of(L'\\'));
}
