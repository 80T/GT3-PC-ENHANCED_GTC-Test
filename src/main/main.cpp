/* main.cpp - the entry point of GT3 PC.
 *
 * On the PS2 the bootstrap loaded CORE.GT3 and crt0 ran its main on the
 * main thread.  Here WinMain opens the log, finds the game files, opens the
 * window, starts the platform services and runs the game's main
 * (gt3_main, src/game/app/T000_main.cpp) on the first game thread, then
 * pumps the window's messages until it closes. */
#include <windows.h>
#include <shellapi.h>                       /* CommandLineToArgvW (the Esc menu's Restart) */

#include "pc_static_init.h"
#include "engine/media/native/still_cache.h"
#include "platform/audio.h"
#include "platform/crash.h"
#include "platform/gamepad.h"
#include "platform/gpu.h"
#include "platform/heap_arena.h"
#include "platform/log.h"
#include "platform/paths.h"
#include "platform/play_time.h"
#include "platform/scheduler.h"
#include "platform/settings.h"
#include "platform/system_menu.h"
#include "platform/window.h"

#include <stdlib.h>
#include <string.h>
#include <string>

int gt3_main(int argc, char **argv);

namespace {

/* The main thread's priority before the game lowers it (T002 drops it to
 * 9 once the render thread, 7, is up). */
constexpr int kMainThreadPriority = 1;
/* The PS2 main thread had a 1 MB stack (crt0 _stack_size); some game
 * functions keep large arrays on it (play_saved_replay 0x41980 bytes), and
 * MSVC's frames are not the egcs ones: room to spare. */
constexpr int kMainThreadStack = 16 * 1024 * 1024;

/* What the bootstrap passed to CORE.GT3. */
char kArg0[] = "cdrom0:\\CORE.GT3;1";
char *g_gameArgv[] = {kArg0, nullptr};

void game_main_thread(void *)
{
    pc_run_static_inits();
    int r = gt3_main(1, g_gameArgv);
    pclog::write("game: main returned %d", r);
    PostMessageW(window::handle(), WM_CLOSE, 0, 0);
}

/* The Esc menu's Restart (platform/system_menu.h): the new run gets this
 * one's arguments and --restart-after <this process id>, and waits for this
 * one to end before anything else - the log rotated, the audio device, the
 * display mode and the memory card files let go.  Restart as new game adds
 * --new-game, for that one run. */
constexpr char kRestartAfter[] = "--restart-after";
constexpr wchar_t kRestartAfterW[] = L"--restart-after";
constexpr char kNewGame[] = "--new-game";
constexpr wchar_t kNewGameW[] = L"--new-game";

void wait_for_previous_run(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], kRestartAfter) != 0)
            continue;
        const DWORD pid = strtoul(argv[i + 1], nullptr, 10);
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (h != nullptr) {
            WaitForSingleObject(h, 15000);
            CloseHandle(h);
        }
        return;
    }
}

/* One argument for the command line (the C runtime's parsing rules:
 * backslashes doubled before a quote). */
void append_arg(std::wstring &cmd, const wchar_t *arg)
{
    if (!cmd.empty())
        cmd += L' ';
    if (*arg != L'\0' && wcspbrk(arg, L" \t\"") == nullptr) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    int slashes = 0;
    for (const wchar_t *p = arg;; p++) {
        if (*p == L'\\') {
            slashes++;
            continue;
        }
        if (*p == L'\0' || *p == L'"')
            cmd.append(*p == L'"' ? slashes * 2 + 1 : slashes * 2, L'\\');
        else
            cmd.append(slashes, L'\\');
        slashes = 0;
        if (*p == L'\0')
            break;
        cmd += *p;
    }
    cmd += L'"';
}

void start_new_run(bool newGame)
{
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring cmd;
    append_arg(cmd, exe);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv != nullptr && i < argc; i++) {
        if (wcscmp(argv[i], kRestartAfterW) == 0) {
            i++;                                /* (the last run's) */
            continue;
        }
        if (wcscmp(argv[i], kNewGameW) == 0)
            continue;                           /* (the last run's: a Restart from it loads as usual) */
        append_arg(cmd, argv[i]);
    }
    if (argv != nullptr)
        LocalFree(argv);
    if (newGame)
        append_arg(cmd, kNewGameW);
    append_arg(cmd, kRestartAfterW);
    append_arg(cmd, std::to_wstring(GetCurrentProcessId()).c_str());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        return;
    AllowSetForegroundWindow(pi.dwProcessId);   /* its window in front */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc = __argc;
    char **argv = __argv;
    wait_for_previous_run(argc, argv);

    char exe[MAX_PATH * 2];
    GetModuleFileNameA(nullptr, exe, sizeof(exe));
    char *slash = strrchr(exe, '\\');
    if (slash) {
        /* the last run's log is kept (a crash report survives a restart) */
        char prev[MAX_PATH * 2];
        strcpy(slash + 1, "gt3pc.prev.log");
        strcpy(prev, exe);
        strcpy(slash + 1, "gt3pc.log");
        MoveFileExA(exe, prev, MOVEFILE_REPLACE_EXISTING);
    }
    pclog::open(exe);
    pclog::write("Gran Turismo 3 A-Spec (PC)");
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], kNewGame) == 0) {
            system_menu::set_new_game_run();
            pclog::write("restart: a new game: no autoload");
        }
    crash::install();
    {
        uint32_t heapSize;
        heap_arena(&heapSize);  /* (before the libraries split the address space up) */
    }

    if (!paths::init(argc, argv)) {
        /* PC (2026-11-14): a disc image that could not be used (its path is
         * UTF-8: any file's name) */
        if (!paths::init_error().empty()) {
            const std::string &e = paths::init_error();
            std::wstring w((size_t)MultiByteToWideChar(CP_UTF8, 0, e.c_str(), (int)e.size(), nullptr, 0), L'\0');
            if (!w.empty())
                MultiByteToWideChar(CP_UTF8, 0, e.c_str(), (int)e.size(), &w[0], (int)w.size());
            MessageBoxW(nullptr, w.c_str(), L"Gran Turismo 3", MB_OK | MB_ICONERROR);
            return 1;
        }
        MessageBoxA(nullptr,
                    "The game files were not found (the data folder next to gt3pc.exe).\n\n"
                    "Run the GT3 launcher to install them from your Gran Turismo 3 A-Spec disc image.",
                    "Gran Turismo 3", MB_OK | MB_ICONERROR);
        return 1;
    }

    settings::load();   /* gt3pc.ini: the display mode */
    play_time::start(); /* gt3pc.ini [Stats] PlayTime: counted from here */
    if (settings::debug_console())
        pclog::open_console();  /* (before the window: the game window takes the focus after it) */
    gt3media::still_cache_prepare();    /* the arcade menus' stills: no disk or decoder in the menus */
    if (!window::create("Gran Turismo 3 A-Spec"))
        pclog::fatal("could not create the window");
    if (!gpu::init(window::handle()))
        pclog::fatal("Direct3D 11 could not start on this PC");

    gamepad::init();    /* the game controllers (SDL3); keyboard only without them */
    sched::init();
    audio::init();      /* the native SPU2 (snd_native_init) and the sound output (on sched's clock) */
    sched::start_main_thread(game_main_thread, nullptr, kMainThreadPriority, kMainThreadStack);

    int code = window::run_message_loop();
    pclog::write("window closed");
    /* where the game was (a softlock closed with the window: its evidence) */
    sched::log_threads("the window closed");
    gamepad::shutdown();    /* the motors stop */
    audio::shutdown();      /* the output stopped and silent (its looped buffer played on while the process
                               ended: a crackle at the close), the dump closed */
    play_time::stop();      /* the count written */
    const bool restart = system_menu::restart_requested();
    if (restart)
        pclog::write("restart: a new run starts once this one has ended");
    pclog::close();
    if (restart)
        start_new_run(system_menu::restart_as_new_game());
    /* The game threads never end by themselves. */
    ExitProcess(code);
}
