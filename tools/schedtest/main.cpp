/* schedtest - checks the game-thread scheduler (src/platform/scheduler.cpp)
 * against the EE kernel rules the game relies on:
 *   1. a higher-priority thread made ready by SignalSema runs at once;
 *   2. iSignalSema / signal_sema_helper does not switch until the next
 *      switch point (a kernel call or EI);
 *   3. equal priorities run FIFO; RotateThreadReadyQueue rotates;
 *   4. SleepThread / WakeupThread count wakeups;
 *   5. interrupt handlers (vblank) run, never inside a DI section, and a
 *      thread they wake preempts at the next switch point;
 *   6. ChangeThreadPriority returns the old priority and reschedules;
 *   7. DelayThread waits about the time asked;
 *   8. only one game thread runs at a time (a shared counter without locks);
 *   9. threads woken in one interrupt run by priority, not in wake order.
 * Exit code 0 when everything passes. */
#include "eekernel.h"
#include "platform/log.h"
#include "platform/scheduler.h"

#include <chrono>
#include <thread>
#include <stdio.h>
#include <atomic>
#include <string>
#include <vector>

static std::vector<std::string> g_trace;
static int g_failures;
static std::atomic<int> g_running{0};
static std::atomic<bool> g_done{false};

static void trace(const char *s)
{
    if (g_running.fetch_add(1) != 0) {
        printf("FAIL: two game threads ran at once (%s)\n", s);
        g_failures++;
    }
    g_trace.push_back(s);
    g_running.fetch_sub(1);
}

static void expect(const char *what, const std::vector<std::string> &want)
{
    bool ok = g_trace == want;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        g_failures++;
        printf("   got:");
        for (auto &s : g_trace) printf(" %s", s.c_str());
        printf("\n  want:");
        for (auto &s : want) printf(" %s", s.c_str());
        printf("\n");
    }
    g_trace.clear();
}

static int make_sema(int init, int max = 1)
{
    SemaParam p = {};
    p.initCount = init;
    p.maxCount = max;
    return CreateSema(&p);
}

static int make_thread(void (*fn)(void *), int prio)
{
    ThreadParam p = {};
    p.entry = fn;
    p.initPriority = prio;
    p.stackSize = 0x10000;
    return CreateThread(&p);
}

/* ---- 1 / 2: SignalSema vs iSignalSema --------------------------------- */
static int s_sema;
static void high_waiter(void *)
{
    trace("high-wait");
    WaitSema(s_sema);
    trace("high-run");
}

static void test_signal()
{
    s_sema = make_sema(0);
    int t = make_thread(high_waiter, 5);
    StartThread(t, nullptr);                /* prio 5 beats us (10): runs to its wait */
    trace("main-signal");
    SignalSema(s_sema);                     /* high wakes and runs at once */
    trace("main-after");
    expect("SignalSema switches to a higher-priority waiter at once",
           {"high-wait", "main-signal", "high-run", "main-after"});

    t = make_thread(high_waiter, 5);
    StartThread(t, nullptr);
    trace("main-isignal");
    iSignalSema(s_sema);                    /* no switch yet */
    trace("main-still");
    DI();
    EI();                                   /* EI is a switch point */
    trace("main-after");
    expect("iSignalSema waits for the next switch point (EI)",
           {"high-wait", "main-isignal", "main-still", "high-run", "main-after"});
}

/* ---- 3: FIFO at equal priority, rotation ------------------------------ */
static void named(void *arg)
{
    trace((const char *)arg);
}

static void test_fifo()
{
    int a = make_thread(named, 10), b = make_thread(named, 10);
    StartThread(a, (void *)"A");
    StartThread(b, (void *)"B");            /* same priority as main: not run yet */
    trace("main");
    RotateThreadReadyQueue(10);             /* main to the back: A, B, then main */
    trace("main-back");
    expect("equal priority is FIFO; rotation lets the others run",
           {"main", "A", "B", "main-back"});
}

/* ---- 4: sleep / wakeup counting ----------------------------------------- */
static int s_sleeper;
static void sleeper(void *)
{
    trace("sleep1");
    SleepThread();                          /* a wakeup is pending: no wait */
    trace("sleep2");
    SleepThread();                          /* waits */
    trace("woken");
}

static void test_sleep()
{
    s_sleeper = make_thread(sleeper, 20);   /* lower than main */
    StartThread(s_sleeper, nullptr);
    WakeupThread(s_sleeper);                /* counted: it has not slept yet */
    trace("main-sleeps");
    ChangeThreadPriority(GetThreadId(), 30);    /* let it run */
    trace("main-back");
    WakeupThread(s_sleeper);
    trace("main-end");
    ChangeThreadPriority(GetThreadId(), 10);
    expect("SleepThread / WakeupThread count wakeups",
           {"main-sleeps", "sleep1", "sleep2", "main-back", "woken", "main-end"});
}

/* ---- 5: interrupts ---------------------------------------------------------- */
static std::atomic<int> s_vblanks{0};
static std::atomic<int> s_inDI{0};
static std::atomic<int> s_badIntr{0};
static int s_vsSema;
static int vblank_handler(int)
{
    if (s_inDI)
        s_badIntr++;
    s_vblanks++;
    iSignalSema(s_vsSema);
    return 0;
}

static void vsync_waiter(void *)
{
    for (int i = 0; i < 3; i++) {
        WaitSema(s_vsSema);
        trace("vs");
    }
}

static void test_intr()
{
    s_vsSema = make_sema(0, 1000);
    int id = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    int t = make_thread(vsync_waiter, 5);
    StartThread(t, nullptr);
    /* A busy section with interrupts off: no handler may run inside. */
    for (int k = 0; k < 20; k++) {
        DI();
        s_inDI = 1;
        double until = sched::now_seconds() + 0.005;
        while (sched::now_seconds() < until) {
        }
        s_inDI = 0;
        EI();
    }
    DelayThread(80000);
    trace("main");
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, id);
    expect("vblank handlers wake a higher-priority thread",
           {"vs", "vs", "vs", "main"});
    printf("%s no handler ran inside a DI section (%d vblanks)\n", s_badIntr ? "FAIL" : "ok  ", (int)s_vblanks);
    if (s_badIntr)
        g_failures++;
}

/* ---- 9: several threads woken in one interrupt ----------------------------- */
static std::atomic<int> s_armed{0};
static int s_lowSema, s_highSema, s_doneSema;
static int wake_two_handler(int)
{
    if (s_armed.exchange(0)) {
        iSignalSema(s_lowSema);             /* the low one is woken first */
        iSignalSema(s_highSema);
    }
    return 0;
}

static void low_waiter(void *)
{
    WaitSema(s_lowSema);
    trace("low");
    SignalSema(s_doneSema);
}

static void high_waiter2(void *)
{
    WaitSema(s_highSema);
    trace("high");
    SignalSema(s_doneSema);
}

static void test_intr_order()
{
    s_lowSema = make_sema(0);
    s_highSema = make_sema(0);
    s_doneSema = make_sema(0, 2);
    int id = AddIntcHandler(INTC_VBLANK_S, wake_two_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    StartThread(make_thread(low_waiter, 20), nullptr);
    StartThread(make_thread(high_waiter2, 3), nullptr);    /* runs to its wait */
    DelayThread(1000);                      /* the low one runs to its wait */
    s_armed = 1;
    WaitSema(s_doneSema);                   /* every game thread waits: CPU idle */
    WaitSema(s_doneSema);
    trace("main");
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, id);
    expect("threads woken in one interrupt run by priority, not wake order",
           {"high", "low", "main"});
}

/* ---- 6 / 7 ------------------------------------------------------------------ */
static void test_misc()
{
    int old = ChangeThreadPriority(GetThreadId(), 12);
    int back = ChangeThreadPriority(GetThreadId(), 10);
    bool ok = old == 10 && back == 12;
    printf("%s ChangeThreadPriority returns the old priority\n", ok ? "ok  " : "FAIL");
    if (!ok)
        g_failures++;
    double t0 = sched::now_seconds();
    DelayThread(30000);
    double dt = sched::now_seconds() - t0;
    ok = dt > 0.029 && dt < 0.045;
    printf("%s DelayThread(30 ms) took %.1f ms\n", ok ? "ok  " : "FAIL", dt * 1000);
    if (!ok)
        g_failures++;
}

static void game_main(void *)
{
    ChangeThreadPriority(GetThreadId(), 10);
    test_signal();
    test_fifo();
    test_sleep();
    test_intr();
    test_intr_order();
    test_misc();
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    g_done = true;
    SleepThread();
}

int main()
{
    sched::init();
    sched::start_main_thread(game_main, nullptr, 1, 1 << 20);
    for (int i = 0; i < 200 && !g_done; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return g_failures ? 1 : 0;
}
