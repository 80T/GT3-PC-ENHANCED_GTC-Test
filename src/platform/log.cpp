/* log.cpp - the GT3 PC log (log.h). */
#include "platform/log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <mutex>

namespace pclog {
namespace {

std::mutex g_mutex;
FILE *g_file = nullptr;
char g_path[MAX_PATH * 2];
bool g_console = false;

/* Ctrl+C / Ctrl+Break in the console: ignored (the game closes itself) */
BOOL WINAPI console_ctrl(DWORD type)
{
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

void emit(const char *text, int len)
{
    if (len <= 0)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fwrite(text, 1, len, g_file);
        fflush(g_file);
    }
    if (g_console) {
        fwrite(text, 1, len, stdout);
        fflush(stdout);
    }
    char buf[2048];
    int n = len < (int)sizeof(buf) - 1 ? len : (int)sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    OutputDebugStringA(buf);
}

} // namespace

void open(const char *path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        fclose(g_file);
    g_file = fopen(path, "wb");
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_console = GetStdHandle(STD_OUTPUT_HANDLE) != nullptr &&
                GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_UNKNOWN;
}

void open_console()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_console || !AllocConsole())
        return;
    SetConsoleTitleW(L"Gran Turismo 3 A-Spec - debug output");
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    /* a long scrollback; no close button, no mouse selection (above) */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(out, &info) && info.dwSize.Y < 9999) {
        COORD size = {info.dwSize.X, 9999};
        SetConsoleScreenBufferSize(out, size);
    }
    if (HWND w = GetConsoleWindow())
        DeleteMenu(GetSystemMenu(w, FALSE), SC_CLOSE, MF_BYCOMMAND);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(in, &mode))
        SetConsoleMode(in, (mode & ~(DWORD)ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
    FILE *f = nullptr;
    if (freopen_s(&f, "CONOUT$", "w", stdout) != 0)
        return;
    /* what the log holds so far */
    if (g_file != nullptr) {
        fflush(g_file);
        if (FILE *r = fopen(g_path, "rb")) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), r)) > 0)
                fwrite(buf, 1, n, stdout);
            fclose(r);
            fflush(stdout);
        }
    }
    g_console = true;
}

void close()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file)
        fclose(g_file);
    g_file = nullptr;
}

void vwrite(const char *fmt, va_list ap)
{
    char buf[4096];
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    if (n == 0 || buf[n - 1] != '\n')
        buf[n++] = '\n';
    emit(buf, n);
}

void write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwrite(fmt, ap);
    va_end(ap);
}

void raw(const char *text, int len)
{
    emit(text, len);
}

void fatal(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write("FATAL: %s", buf);
    MessageBoxA(nullptr, buf, "Gran Turismo 3", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}

} // namespace pclog
