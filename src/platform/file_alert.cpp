/* file_alert.cpp - the pop-up of files in use by another program
 * (file_alert.h). */
#include "platform/file_alert.h"
#include "platform/gpu.h"

#include <windows.h>
#include <process.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace file_alert {
namespace {

constexpr int kTextControl = 0xFFFF;        /* (a message box's text) */

HANDLE g_thread;
std::atomic<DWORD> g_threadId{0};
std::string g_text;                         /* what the thread shows */
std::vector<std::string> g_named;           /* the files of the last call */

std::string text_of(const std::vector<std::string> &files)
{
    std::string t = files.size() == 1 ? "This game file is in use by another program:\n\n"
                                      : "These game files are in use by another program:\n\n";
    for (const std::string &f : files)
        t += f + "\n";
    t += "\nThe game is waiting, and goes on by itself as soon as ";
    t += files.size() == 1 ? "it is free." : "they are free.";
    t += "\n\nClose the file in that program. An SQLite database (.sqlite3) is read under SQLite's own locks: an "
         "editor connected to it does not hold it up, only a change under way does - commit or roll it back there.";
    return t;
}

unsigned __stdcall box_thread(void *)
{
    g_threadId = GetCurrentThreadId();
    MessageBoxA(nullptr, g_text.c_str(), "Gran Turismo 3", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    return 0;
}

bool box_open()
{
    return g_thread != nullptr && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT;
}

BOOL CALLBACK find_box(HWND h, LPARAM out)
{
    char cls[16];
    if (GetClassNameA(h, cls, sizeof(cls)) != 0 && strcmp(cls, "#32770") == 0) {
        *reinterpret_cast<HWND *>(out) = h;
        return FALSE;
    }
    return TRUE;
}

/* The box's window (none for the moment after its thread starts). */
HWND box_window()
{
    HWND h = nullptr;
    const DWORD id = g_threadId.load();
    if (id != 0)
        EnumThreadWindows(id, find_box, reinterpret_cast<LPARAM>(&h));
    return h;
}

void open_box(const std::vector<std::string> &files)
{
    g_text = text_of(files);
    g_threadId = 0;
    g_thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, box_thread, nullptr, 0, nullptr));
}

/* Closed, its thread ended.  (WM_CLOSE: a box's lone OK button has the id
 * IDCANCEL, and a WM_COMMAND of IDOK does nothing.) */
void close_box()
{
    if (g_thread == nullptr)
        return;
    for (int i = 0; i < 400 && box_open(); i++) {
        if (HWND h = box_window())
            PostMessageA(h, WM_CLOSE, 0, 0);
        Sleep(5);
    }
    WaitForSingleObject(g_thread, 1000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

} // namespace

void show(const std::vector<std::string> &files)
{
    if (files == g_named)
        return;
    const bool added = std::any_of(files.begin(), files.end(), [](const std::string &f) {
        return std::find(g_named.begin(), g_named.end(), f) == g_named.end();
    });
    if (files.empty() || added) {
        close_box();
        if (!files.empty())
            open_box(files);
    } else if (box_open()) {
        if (HWND h = box_window())
            SetDlgItemTextA(h, kTextControl, text_of(files).c_str());
    }
    g_named = files;
}

void leave_fullscreen()
{
    gpu::leave_fullscreen();
}

} // namespace file_alert
