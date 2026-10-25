# GT3 PC: the system side

A survey of what the decompiled program (`decomp/core_gt3enhanced`) asks of
the PlayStation 2 system: threads, interrupts and timing, the kernel and
SDK calls, file I/O, controllers, the memory card, system configuration and
boot, and the memory map. Each part ends with what the Windows platform
layer must provide. [PORT.md](PORT.md) has the overall design; the C and
C++ runtime is in [runtime.md](runtime.md).

Paths (`src/...`, `include/...`) are relative to `decomp/core_gt3enhanced`.
`file:line` points at the function or the statement. Thread priorities are
EE kernel priorities: smaller is more urgent (0 highest, 127 lowest).

Scratch scripts that produced the counts and call lists are in
`build/scratch/system/` (`kcalls.py`, `callgraph.py`, `callers.py`,
`spins.py`, `gnu.py`, `ctors.py`, `permissive.sh`); `sdkcalls.txt` there is
the full list of SDK / runtime functions with every caller.

## 1. Threads

### 1.1 The EE kernel rules the code was written for

- One CPU. The running thread keeps it until it blocks (`WaitSema`,
  `SleepThread`, a vsync or DMA wait, which are `SleepThread` inside) or a
  thread of higher priority becomes ready.
- A thread made ready by a non-interrupt call (`SignalSema`, `WakeupThread`,
  `StartThread`, `ChangeThreadPriority`) preempts the caller at once when it
  is more urgent. A thread made ready by an `i` call (`iSignalSema`,
  `iWakeupThread`) runs when the interrupt returns, or, when the `i` call
  was made from thread context (see `signal_sema_helper` below), at the next
  rescheduling point: the next interrupt or the caller's next kernel call.
- Interrupts preempt anything that has not disabled them. An interrupt that
  readies a more urgent thread switches to it on return: the render thread
  takes the CPU from the main thread at every vblank, wherever the main
  thread is.
- No time slicing. Threads of equal priority run in FIFO order; a thread
  that becomes ready goes behind the ready threads of its priority. Only
  `RotateThreadReadyQueue` moves a running thread behind its equals (used
  once, by `yield_low_priority`).
- `SleepThread` / `WakeupThread` count: a wakeup sent to a thread that is not
  sleeping is remembered (wakeup count) and the next `SleepThread` returns at
  once. `CancelWakeupThread` clears the count. Several waits depend on this
  (`file_req_result`, `gfx_wait_vblank_event`, `CarIconMaker::run`).
- VU0 registers and the scratchpad are not saved on a thread switch. Code
  that uses them over several statements disables interrupts around them
  (the cameras, `src/game/camera/T397..T406`, and every pgl matrix user).

### 1.2 The helper layer every thread goes through

`src/engine/system/thread_util.c` and `src/engine/pdi/pdistd_misc.c` (the
`ThreadCtx` record `{thread, sema}` is in
`include/engine/media/G002_mpeg_frameq.h:76`):

| Function | file:line | What it does |
|---|---|---|
| `intr_suspend` | thread_util.c:52 | `di` (R5900 errata loop), returns 1 if interrupts were on. Paired with the inline `intr_resume(was)` (`G002_mpeg_frameq.h:614`) or a raw `ei` / `EI()`. |
| `create_mutex_sema(init)` | thread_util.c:69 | `CreateSema{max 1, init}`; retries with `yield_low_priority` while it fails; an id of 0 is thrown away (recursion): ids must never be 0 natively. |
| `yield_low_priority` | thread_util.c:93 | drop to priority 0x7E, 100 x (`busy_wait_delay(10)` + `RotateThreadReadyQueue(0x7E)`), restore the priority read with `ReferThreadStatus`. Lets every other ready thread run. 24 callers, all retry loops. |
| `signal_sema_helper(sid)` | thread_util.c:119 | `iSignalSema` from thread context: signals but does not reschedule. 43 callers; nearly every "signal" in the game is this one. |
| `thread_create_retry` | pdistd_misc.c:28 | `CreateThread` until it does not return -1 (yielding). |
| `thread_start_retry` | thread_util.c:131 | the above, then `StartThread` until it returns the id. |
| `worker_thread_start` | thread_util.c:144 | `sema = create_mutex_sema(0)`, then `thread_start_retry`. |
| `worker_thread_join` | thread_util.c:155 | `WaitSema(sema)`, `DeleteSema(sema)`. |
| `worker_thread_exit` | thread_util.c:165 | raise self to priority 1, `signal_sema_helper(sema)`, `ExitDeleteThread`. |
| `busy_wait_delay(n)` | thread_util.c:184 | 75n + 2 passes of a 6-nop loop (MIPS asm). Natively a no-op or `_mm_pause()`. |
| `global_lock_acquire/release` | pdistd_misc.c:53 / 67 | lazily created mutex around the `host:` fileio wrappers. |

### 1.3 Thread table

Every thread the program creates. "Stack" is the PS2 size; natively each
game thread needs a real stack sized for MSVC code (section 8.5).

| Thread | Created by (file:line) | Entry | Prio | PS2 stack | Purpose | Waits on / woken by |
|---|---|---|---|---|---|---|
| main | crt0 `InitMainThread` (`src/runtime/startup/crt0.S`), `main` at `src/game/app/T000_main.cpp:105` | `main` | 1 until `render_thread_start` (`T002_render_thread.cpp:190`) drops it to **9** | 1 MB (0x01F00000..0x02000000) | boot (`game_boot`), memory card warning, opening movie, menus, race flow | `gfx_vsync` waits (140 calls in game code); `SleepThread` while a race runs (`RaceBase::virtual_38`, `T129_race_base.cpp:847`, woken by `RaceBase::signal` from the render thread), in `RenderBase::sleep` (`T246_render_base.cpp:89`, woken by a Timer object on the render thread) and in `CarIconMaker::run` (`T184_car_icon_maker.cpp:85`); `file_req_result` (`T503_vol_thread.cpp:287`) |
| libkernl top thread | `InitThread` (`src/sdk/libkernl/thread.c:73`) from `_InitSys` | `topThread` | 0 | 0x400 | performs `iWakeupThread` / `iRotateThreadReadyQueue` / `iSuspendThread` aimed at the current thread (EE kernel bug work-around) | not needed natively |
| render | `render_thread_start` (`src/game/app/T002_render_thread.cpp:174`, `CreateThread` :188) | `render_thread_main` :47 | **7**; 1 while it waits for the vblank (:83, :93) | 0x8000 static | per frame: `mc_manager_tick`, `PadManager::update`, the current GameObject's update + render, `gfx_sync_pipeline`, `gfx_vsync(-1)`, buffer flip, `gfx_kick_frame_lists` | `WaitSema(g_00350B20)` once at start (released by `render_thread_release` :196); `gfx_vsync` / `gfx_dma_sync` sleeps woken by the vblank and DMAC handlers; `WakeupThread` of the exit waiter |
| vol reader | `vol_init` (`src/engine/io/T503_vol_thread.cpp:414`, `worker_thread_start` :428) | `vol_thread_main` :113 | **4**; 9 while inflating (:159) | 0x8000 memalign | one file request at a time: reads in 0x40000 pieces through `pdipipe` or inflates `.gz` (GZIP::InflateLoader), wakes the waiters | `WaitSema(vt->sema)` per request (`file_req_wait` :277 signals it); `WakeupThread` of each `FileWaiter` thread |
| memory-card worker | `mc_manager_init` (`src/engine/io/T501_memorycard_manager.cpp:329`, :355) | `mc_thread_main` :241 | **4** | 0xC00 inside `McManager` | card polls (every 4th frame, queued by `mc_manager_tick` :406 from the render thread) and the async requests | `WaitSema(req_sema)`; `lock_sema` around every card operation; `WakeupThread` of `mc_async_wait` waiters (no callers) |
| pad | `padStart` (`src/engine/input/T477_ps_port.cpp:261`, :273) | `padThreadMain` :93 | **4** | 0x1000 static | reads ports 1 and 2, sets DualShock mode (locked), pressure mode, actuators | `WaitSema(thread->sema)`, signalled once a frame by `padWakeThread` :309 (render thread, `signal_sema_helper`) |
| music sequencer | `music_init` (`src/game/sound/T463_music_seq.cpp:97`, :129) | `music_thread` :81 | **6** | 0x1000 static | one tick per frame: `seq_update`, `snd_flush`, `pdibgm_update`, `clock_update` | `WaitSema(g_musicVblankSema)`, signalled by the `INTC_VBLANK_E` handler `music_vblank_handler` :71 |
| loader | `loader_thread_start` (`src/engine/io/T508_loader_thread.cpp:76`), started and stopped by `gtmode_main`, `run_arcade_menu`, `arcade_preview_test` | `loader_thread_main` :46 | **0x10** | 0x1000 static | background loads for the menus (`AsyncLoader`, T507) and deferred frees (`loader_defer`) | `gfx_vsync(2)` each idle pass; command word `g_00352FB0` polled; `file_read_blocking` |
| memory-card file manager (one per card menu) | `MemorycardFileManager::createThread` (`src/game/menu/memorycard/T109_memorycard_file_manager.cpp:278`) | `threadMain` :373 | **0x10** | 0x8000 inside the object | lists and reads the saves of one card for the card screens | `gfx_vsync(1)` / `gfx_vsync(10)`; state words polled by the menu (`pause` / `restart` / `stop`) |
| logger recorder (debug logger only) | `logger_record_start` (`src/game/debug/T055_logger.cpp:573`) | `logger_record_thread` :493 | **10** | 0x4000 on the main thread's stack (`LoggerControl::run`) | runs `race->update()` as fast as it can and samples two cars | none: a CPU loop with `intr_suspend` sections (risk R7) |
| i.LINK session | `F_0022D870` (`src/engine/system/T473_link_session.cpp:536`), from `RaceLinkBattle::virtual_8` | `link_thread` :559 | **4** | 0x1000 memalign | frame exchange with other consoles | `WaitSema(g_linkGoSema)`; stub natively |
| USB RPC server | `usbd_init(1)` (`src/engine/input/usbd.c:83`) | `usbd_rpc_thread` :187 | **1** | 0x1000 | the 'PUSB' EE server (`sceSifRpcLoop`) | SIF; stub natively |
| USB dispatcher | same | `usbd_dispatch_thread` :173 | **1** | 0x1000 | USB wheel events | `WaitSema(g_usbd_wake_sema)`, signalled by `usbd_vblank_handler` :160 |
| i.LINK RPC server | `pdi1394_init(1)` (`src/engine/pdi/pdi1394_rpc.c:125`) | `pdi1394_server_thread` :154 | **1** | 0x1000 static | the 'PFWE' EE server | SIF; stub natively |
| movie feeder | `mpegif::startThread` (`src/engine/media/T529_mpegif.cpp:190`, `CreateThread` :207) | `mpegif::threadMain` :313 | **10** (`prioFeed`); raises itself to 1 (`prioHigh`) and 4 (`prioRead`) | 0xC00 **in the scratchpad** (0x70001700) | reads the PSS stream, feeds the decoder | movie agent |
| movie decoder | `mpeg_player_create` (`src/engine/media/mpeg_player.c:219`, :245) | `mpeg_player_thread` :306 | **11** (`prioDecode`) | 0xC00 **in the scratchpad** (0x70000B00) | MPEG-2 decode with the IPU | `SleepThread` woken by the IPU interrupt / DMAC handlers; movie agent |
| (libcdvd callback thread) | `sceCdInitEeCB` (`src/sdk/libcdvd/libcdvd.c:262`) | `_sceCdCallbackThread` | caller's | caller's | no caller | not needed |

`engine/sound` (`sound_driver.c`, `sound_seq.c`) creates no thread: it runs
on the music thread and on its callers' threads, guarded by
`intr_suspend`. The sound and movie IOP-side work (pdispu2, pdibgm,
pdimpeg) becomes native service threads outside the game scheduler
(PORT.md).

The movie priorities come from `MenuBgMovie::open`
(`src/game/menu/framework/T253_menu_bg_movie.cpp:48`): control 1, read 4,
feed 10, decode 11.

Priority order at run time, most urgent first: render waiting for vblank
(1), USB / i.LINK servers (1, stubbed), pad / vol / mc worker / link (4),
music (6), render (7), main and vol-while-inflating (9), logger (10), movie
feeder (10) and decoder (11), loader and card file managers (0x10), any
thread inside `yield_low_priority` (0x7E).

### 1.4 How they synchronise

- **Main and render share the game objects without locks.** Main hands an
  object to the render thread with `render_set_object(obj)`
  (`T002_render_thread.cpp:226`: `PollSema` on the render semaphore) and
  takes it back with `render_set_object(NULL)` (`signal_sema_helper`). The
  render thread runs `obj->update()` and `obj->render()` every frame; main
  sleeps or waits for vsyncs meanwhile. A race is entirely driven by the
  render thread (`RacePS2Base::virtual_41`, `T130_race_ps2_base.cpp:214`:
  `render_set_object(this)`, then main sleeps until `RaceBase::signal`).
- **Data shared between threads is protected by interrupt-off sections**,
  not by semaphores: the menu object tree (`T220_object.cpp`, 10 sections),
  the race event queue (`T122`), the file request record and waiter list
  (`T503`), the loader ring (`T508`), the card slots (`T501`), the clock
  dates (`T465`), the pgl matrix stack and VU0 (cameras). 62 sections in game
  code, 62 in the engine, plus 25 `DIntr`/`EIntr` pairs in pgl and media.
- **Semaphores** guard resources (VRAM allocator `T513`, PadManager's
  Control list `T475`, the RPC objects `T527`, the card lock `T501`,
  `mcard_io.c`'s libmc lock, the music lock `T463`, the scf lock) and signal
  work (vol, mc, pad, music, link threads).
- **Sleep / wakeup** for one-shot waits (file results, race end, menu
  timers, vsync and DMA waits).
- **`ChangeThreadPriority`** to run a step urgently: render (1 during the
  vsync wait), vol (9 during inflate), `worker_thread_exit` (1), mpegif
  (1 / 4 / 10, and it uses the **return value** as the previous priority,
  `T529_mpegif.cpp:106`, `mpeg_player.c:714`).
- **Polled flags with a vsync wait in the loop** between the menu thread and
  the lower-priority loader / card threads (`loader_idle`, the card manager
  state words): they work because the waiting thread blocks in `gfx_vsync`.

### 1.5 Scheduling assumptions the code relies on

1. A thread woken at interrupt time preempts lower-priority threads
   immediately (render at vblank; IPU / DMA waiters). Without it the frame
   is late, but nothing deadlocks as long as the running thread reaches a
   rescheduling point soon (1.7).
2. `iSignalSema` from thread context does **not** switch at once: the
   signaller goes on until its next kernel call. `file_req_wait` +
   `file_req_result` (`T503:277`, `:287`), `padWakeThread` and
   `render_set_object(NULL)` are written for this, but they also work if
   the switch happens early (checked: the waiter is registered inside the
   interrupt-off section, and a result that is already there is taken).
3. Equal priorities do not time-slice: two priority-4 threads (pad, vol, mc
   worker) never interleave inside a step unless one blocks.
4. Interrupt-off sections exclude both interrupts and thread switches.
5. Wakeup counting (1.1).
6. `ChangeThreadPriority` returns the previous priority.
7. `PollSema` returns the semaphore id when it took it (`T473:338`,
   `usbd.c:162`); `SignalSema` on a full binary semaphore leaves it at 1.
8. During `game_init` the main thread runs at priority **1**, above every
   worker it starts (vol, pad, mc, music: 4..6). They only run while main
   blocks (its file reads).

### 1.6 Busy waits and deadlock risks

Loops that end only because another thread or an interrupt runs, or never.
"PS2" says what ends them on the console; "Native" what happens under a
scheduler that switches only at kernel calls.

| # | file:line | Loop | PS2 | Native risk and fix |
|---|---|---|---|---|
| R1 | `src/engine/io/T503_vol_thread.cpp:241` (`file_request`), `:384` (`file_read_sound_bank`); `T504_file_queue.cpp:33` (`file_req_alloc`); `T502_file_resolve.cpp:172` (`file_size_retry`); `src/game/app/T006_file_resolve_retry.cpp:13` (`file_resolve_retry`, movies) | `while (file_resolve(...) == -1) ;` | a missing file hangs the console too (only a disc swap could change the TOC) | **hard hang on any missing file**: the TOC is in memory, nothing can change it. Log the path once and fail the request (return NULL / size 0, or treat as empty), since files will be moved between builds. |
| R2 | `src/engine/io/T510_vol_device.cpp:95..97` (`vol_mount`) | `do rec = iso9660_find(name); while (rec == NULL);` | waits for a readable disc | hang if gt3.vol is missing: native `iso9660_find` (4.3) must fail loudly at start-up instead. |
| R3 | `src/engine/pdi/T524_iop_load_module_base.cpp:172`, `:174`, `:188`; `T527_pdi_client_rpc.cpp:17..21`, `:59`; `src/engine/io/mcard_io.c:53` and the 16 other wrappers, `:355` (`mc_sync_wait`) | IOP reboot / module load / RPC bind / RPC call / libmc issue and sync, retried with `yield_low_priority` | the IOP answers | removed with the IOP layer; the native libmc / pipe calls complete synchronously, so the wrappers' loops never iterate. |
| R4 | `src/engine/input/usbd.c:64` (`pjoy_bind`), `src/engine/pdi/pdi1394_rpc.c:170` (`pdi1394_bind`), `:415`; `src/sdk/libkernl/initsys_glue.c:62` (`wait_vblank_start`) | bind retries with a poll of INTC_STAT (hardware) | vblank sets the bit | stub USB / i.LINK; never call `wait_vblank_start`. |
| R5 | `src/engine/pdi/pdi1394_rpc.c:494`, `:497` | `AddIntcHandler` / `RemoveIntcHandler` retried until not -1 | kernel | stubbed with i.LINK. |
| R6 | `src/game/menu/framework/T220_object.cpp:49..56` (`Object::operator new`) | allocate from the menu heap, `yield_low_priority`, again, forever | another thread (the loader's deferred `menu_car_free_buffer`) frees memory | **hang when a menu heap is exhausted** (larger assets, MSVC object sizes). Log and fail after a bounded number of tries. |
| R7 | `src/game/debug/T055_logger.cpp:543` (`logger_record_thread`, priority 10) | `while (!stop && race->takeSignal() != END) race->update();` | the vblank interrupt preempts it for the render thread | CPU loop with no blocking call; it only contains `intr_suspend` / `ei` pairs. Needs preemption at `ei` (1.7) or the render and main threads starve. Debug logger only. |
| R8 | `src/engine/io/T508_loader_thread.cpp:48` (`loader_thread_main`) | `switch` on the command word, an unknown value spins | never happens | none in practice. |
| R9 | `src/game/race/core/T130_race_ps2_base.cpp:224` | `while (virtual_38() != RACE_SIGNAL_END) ;` (each pass `SleepThread`) | render thread's `RaceBase::signal` (`T129:825`) | fine if wakeup counting is exact. |
| R10 | `src/engine/io/T503_vol_thread.cpp:304` (`file_req_result`), `src/engine/pgl/gs_display_driver.c:1158` (`gfx_wait_vblank_event`), `:301` (`gfx_dma_sync`, bounded to 0x78 sleeps), `T184_car_icon_maker.cpp:85` | `SleepThread` until a flag set by the waker | wakeups counted, extra ones cancelled with `CancelWakeupThread` | exact wakeup-count semantics required. |
| R11 | `src/game/sound/T463_music_seq.cpp:81..89` with `music_term` :137 | `music_term` deletes the vblank semaphore without setting `g_musicThreadQuit`, then joins | the thread spins on the deleted semaphore, the join never ends | dead code today (`sound_term` has no caller); if a shutdown path is added, set the quit flag first. |
| R12 | `src/engine/io/T508_loader_thread.cpp:100`, `:105`; `T507_async_loader.cpp:28`; `T109_memorycard_file_manager.cpp:298`, `:301`; `T068_memorycard_control.cpp:581..598`; `T020_arcade_helpers.cpp:97`; `T206_button.cpp:395`; `T207_map_button_list.cpp:253`; `T211_page_manager.cpp:294`, `:866`; `T253_menu_bg_movie.cpp:302`, `:378..383`; `T022_class_select.cpp:377`; `T025_arcade_course_select.cpp:452`; `T051_movie_player.cpp:53` | main waits for the render / loader / card-manager / movie threads with `gfx_vsync(1)` (or `waitFrame`) in the loop | the vsync wait blocks | fine as long as `gfx_vsync` really blocks and the vblank keeps ticking. |
| R13 | `src/engine/pgl/gs_driver_core.c:129` (`gfx_wait_vsync_poll`), `gs_display_driver.c:1098` (`gfx_sync_pipeline`, budgeted VIF / VU1 / GIF polls), `src/engine/media/ipu.c:234` (IPU FIFO spin), `src/engine/input/usbd.c:287` (spins on an event with no handler) | hardware register polls | hardware | owned by the graphics / movie rewrites and the USB stub; must not survive into native code. |
| R14 | `src/sdk/libkernl/sifcmd.c:167`, `sifrpc.c:94`, `:669`, `tty.c:188` | SIF / TTY polls | IOP / DECI2 | dropped with the SDK. |

### 1.7 What the native scheduler must do

- Game threads are native threads; exactly one runs at a time, chosen by
  EE priority (FIFO within a priority). Kernel objects (threads,
  semaphores) live in fixed tables with ids from 1 up (never 0, never -1),
  at most 256 threads and 256 semaphores, constant-initialised so that the
  static constructors (which create semaphores and take the interrupt
  lock, runtime.md 7.4) can use them before `main`.
- Switch points: every kernel call that blocks or readies a thread (with
  the EE rule: a non-`i` call that readies a more urgent thread switches
  immediately; an `i` call defers), **every `ei`** (`intr_resume`, `EIntr`,
  `EI()`, the raw `__asm__("ei")` sites) when a switch is pending, and the
  platform I/O calls (`pdicdvd_read`, `pdipipe_read`: they blocked on the
  IOP on the console). `ei` is where a pending interrupt is taken on the
  EE, so preempting there is faithful; it covers R7 and the allocator
  (every `malloc`/`free` goes through `intr_suspend` / `ei`).
- A thread inside an interrupt-off section is never preempted. If it blocks
  inside one (allowed on the EE: the status register is per thread), the
  section is released while it waits and taken again when it resumes.
- Without true asynchronous preemption the render thread can be late when
  the running game thread computes for a long time without a switch point
  (menu building on the main thread, inflate on the vol thread between
  reads). If that shows, add switch points (a vblank-pending check) in the
  inflate loop and in `file_req_run`, or suspend the running native thread
  from the interrupt thread when it is not inside the platform layer, the
  CRT or an interrupt-off section.
- `TerminateThread` / `DeleteThread` (movies, `usbd_exit`) of a blocked
  thread: park it for good (never wake it); native threads cannot be
  killed safely.
- A thread returning from its entry function behaves as `ExitThread`.

## 2. Interrupts and timing

### 2.1 Handlers

| Cause | Installed by (file:line) | Handler | What it does | Kernel calls |
|---|---|---|---|---|
| INTC_VBLANK_S (2), head | `gfx_init` (`src/engine/pgl/gs_driver_core.c:408`) | `intc_vblank_handler` (`gs_driver.c:257`) | reads and zeroes COP0 PCR0/PCR1 into `perfCount0/1`; `field` = GS CSR.FIELD; `vblankCount++` (64-bit); `vsyncCallback` (never set); wakes every thread on the vsync waiter list and on the three DMA channel lists | `iWakeupThread` |
| INTC_VIF1 (5) | `gfx_init` :409 | `intc_vif1_handler` (`gs_driver.c:299`) | VIF1 stall markers / path sync | none (registers) |
| INTC_GS (0) | `gfx_init` :410 | `intc_gs_finish_handler` (`gs_driver.c:337`) | FINISH count, starts a GS readback | none |
| DMAC VIF0/VIF1/GIF (0..2) | `gfx_dma_init` (`gs_display_driver.c:255..257`) | `dmac_channel_handler` (`gs_driver.c:364`) | chain end / IRQ tags: path sync, readback, completion: `busy = 0` and wakes the channel's waiters | `iWakeupThread` |
| INTC_VBLANK_E (3), tail | `music_init` (`src/game/sound/T463_music_seq.cpp:132`) | `music_vblank_handler` :71 | wakes the music thread | `iSignalSema` (through `signal_sema_helper`) |
| INTC_VBLANK_S, head | `usbd_init` (`src/engine/input/usbd.c:106`) | `usbd_vblank_handler` :160 | if the dispatcher is idle, hand it the lock and wake it | `iPollSema`, `iSignalSema` |
| INTC_VBLANK_S, head, only during the wait | `pdi1394_wait_event` (`src/engine/pdi/pdi1394_rpc.c:494`) | `pdi1394_vblank_handler` :525 | 3 s timeout of an i.LINK wait | `iWakeupThread` |
| INTC_IPU (8) + DMAC FROM_IPU (3) | `ipu_sync_init` (`src/engine/media/ipu.c:434`, `:435`), `ipu_decode_image` (`ipu_image.c:51`, `:52`) | `ipu_sync_intc_handler` (`ipu.c:516`), `ipu_sync_dma_handler` (:531), `ipu_intc_handler` (`ipu_image.c:146`), `ipu_from_dma_handler` (:133) | IPU / DMA completion of the movie decoder and the IPU images | `iDisableIntc`, `iWakeupThread` |
| SIF (SBUS / DMAC SIF0) | libkernl `sceSifInitCmd` | `_sif_cmd_intr_handler` | RPC completion, runs RPC end functions (`PDIClientRpc::endFunc`, `T527:39`: `onCallEnd`, `busy = 0`, `signal_sema_helper`) | removed with the IOP |
| TIM3 alarms | libmc `mc_delay_thread`, libcdvd `_sceCdDelay` | alarm callbacks | delays in the SDK | removed |

Every handler ends with `ExitHandler()` (`sync.l; ei`): natively nothing.
Handlers return 1 (graphics, music) or 0 (USB, i.LINK); with the USB and
i.LINK handlers stubbed there is one handler per cause, so the chain rule
does not matter.

### 2.2 Interrupt-off sections

`intr_suspend` + `intr_resume` (124 sites), `DIntr` + `EIntr` (25, pgl and
media), the inline `DI()` of `sbrk` and `intr_suspend`, and raw
`__asm__("ei")` / `EI()` closes (52 sites: `T501` 18, `T530_allocator` 7,
`T212_menu_car` 5, `T055_logger` 4, `sound_seq.c` 3, `T510` 3, `T477` 3,
`T122` 2, the cameras, `T078`, `T216`, `T264`, `T532`). They protect:

- the pdistd heap (every `Allocator` entry point, `T530_allocator.cpp`) and
  `heap_init_once` (`T532_heap.cpp:54`);
- the menu object tree and its sorted draw list (`T220_object.cpp`), the
  menu heaps' current pointer, `T212_menu_car` reference counts;
- the race event queue (`T122_race_event_queue.cpp`), `RaceBase` signal
  words (`T129`), `T003_game_object_list`;
- VU0 and the pgl matrix stack (cameras `T378`, `T397..T406`, `T401` 5
  sections);
- the file request / waiters (`T503`), the file queue (`T504`), the loader
  ring (`T508`, `loader_run_deferred` runs the deferred calls **with
  interrupts off**), the volume list (`T510`);
- the card slot state (`T501`), pad actuator and state bytes (`T477`), the
  clock dates (`T465`), sound voice tables (`engine/sound`), the menu
  play list (`T085`), sound effects (`T469`);
- vsync / DMA waiter lists (pgl), the movie ring buffers (media).

Sections nest (`mc_slot_status` calls `mc_slot_valid`, both suspend), so the
native lock is recursive per thread. A scan of every section (up to its
`ei`) found no blocking call; the only kernel-visible call inside one is
`printf` in `Allocator::debug` (`T530_allocator.cpp:434`), so the log sink
must work with interrupts off (no scheduling, no game heap).

### 2.3 Vblank and `gfx_vsync`

`gfx_vsync(n)` (`src/engine/pgl/gs_display_driver.c:1184`) is the game's
only clock for frames:

- `n == 0`: returns the low word of `vblankCount` (no caller).
- `n > 0`: waits until n - 1 vblanks have passed since the **last
  `gfx_vsync` call by any thread** (`lastVsync` is global), then for one
  more vblank; returns the vblanks elapsed. `gfx_vsync(1)` = the next
  vblank; `gfx_vsync(2)` = at least two frames since the previous call.
- `n < 0`: if -n vblanks already passed returns 1 at once, else waits until
  they have and returns 0 (the render thread's `gfx_vsync(-1)`).
- Then `lastVsync = vblankCount`, and `ctx->field` = the field sampled by
  the handler when interlaced.

Waiting is `gfx_wait_vblank_event` (:1142): a node on `vsyncWaiters` under
`DIntr`, `SleepThread` while the count has not moved, unlink, then
`CancelWakeupThread`. Main, render, loader, the card managers and the
movie player all share `lastVsync`, so their calls pace each other; native
code keeps this behaviour (the frame logic counts vblanks, PORT.md).

PC (2026-10-20): the vsyncs of every thread but the one running the game
ticks (the render thread, set at its first tick) are the ticks, not the
interrupt: `gfx_tick_vsync` (from `pc_frame_tick_end`) counts a vsync event
and wakes those waiters, who wait for the event count to move. The main
thread's menu loop (`ControlBase::waitFrame`) runs once a vsync and reads
the pads the tick updated; with frames slower than the ticks (a frame cap
below 60, slow frames) two ticks ran back to back and the loop ran twice on
the second's reads, every menu press counting twice (user report). The
interrupt still wakes the render thread at every vblank, and the others too
before the first tick and while the ticks are more than 4 vblanks behind (a
load, a long frame: one event per vblank then), except while the tick step
pauses (`platform/frame_step.h`: the menus' loops pause with the game). A
tick never gives an event ahead of the vblanks; after a stall or a catch-up
the count resyncs (at most one event a tick).

`vblankCount`, `perfCount0/1` are 64-bit and written by the interrupt
thread: on 32-bit x86 they must be read and written atomically (they were
single `ld`/`sd` on the EE).

Native timing: a high-resolution timer at 59.94 Hz raises VBLANK_S
(`intc_vblank_handler`, toggle `field` each time: NTSC interlaced), then
VBLANK_E (the music handler) right after it, then lets the scheduler run
the most urgent ready thread. On the console VBLANK_E came about 1.4 ms
later and preempted the render thread at once, long before the race update
reached `RaceEntryCar::updateSound`; here a woken thread switches in only at
the running thread's next switch point, and the render thread first runs the
frame's D3D work at priority 1 (0.2 .. 10 ms). With the 1.4 ms delay the
music thread's `snd_flush` fell before or after the frame's sound update by
turns (two updates in one flush: sounds stopped and restarted inside it).
Raised with VBLANK_S, the flush runs when the render thread drops to
priority 7, before the update, every frame that met its vsync
(`src/platform/scheduler.cpp`, `kVblankEndDelay`).

### 2.4 Clocks

- The real-time clock comes from the IOP: `pdicdvd_get_time`
  (`src/engine/pdi/T519_pdicdvd.cpp:273`, RPC 'Pcdv' fno 2) returns
  `{low: seconds since 2000-01-01 00:00 in the RTC's time (JST, GMT+9),
  high: stat byte}`. The music thread calls `clock_update`
  (`src/game/sound/T465_clock.cpp:16`) every frame: it keeps the raw
  value and three BCD `CdClock` dates (RTC, GMT, local) published under
  `intr_suspend`. Readers: `clock_get` / `_gmt` / `_local`, the save
  timestamps (`MemorycardDir::setTimeNow`, `T108:236`), records screens.
- `pdicdvd_time_to_clock` (`T519:308`) converts to BCD with a quirk: only
  year 2000 uses the leap-year month table (2024, 2028... are off by one day
  from 29 Feb on). `_gmt` / `_local` go through libscf
  (`sceScfGetGMTfromRTC` = -540 min; `sceScfGetLocalTimefromRTC` = time
  zone + summer time - 540 min) under the scf lock (`scf_lock.c`).
- Native: `pdicdvd_get_time` = `GetSystemTimeAsFileTime` converted to JST
  seconds since 2000-01-01, stat 0. Keep the conversion code; GT3 never
  calls `sceCdReadClock`.

### 2.5 CPU counters

- PCR0 counts CPU cycles (294.912 MHz; `PCCR_GFX` 0x8003783E,
  `pgl_vu1_internal.h:201`), accumulated per vblank. `gfx_perf_counter0`
  (`gs_driver_core.c:89`) only seeds random generators (8 callers, the
  cameras: `T381:12`, `T383:312`, `T384:85`, `T393:59`, `T401:63`,
  `T402:92`, `T405:49`, `T406:47`). `gfx_perf_counter1` has no caller.
  Native: any monotonic 64-bit counter (QPC scaled to 294.912 MHz).
- The COP0 Random register (`mfc0 $1`, the TLB index, 0..47) seeds the race
  random generator (`src/game/race/core/T414_race_random.cpp:26`,
  `randomize`, `raceRandomShuffle`). Native: a value in 0..47 from a fast
  counter.
- No EE timer (T0..T3) is used by game or engine code.

## 3. Kernel and SDK call catalogue

Counts are call sites in `src/game` + `src/engine` (from `ref/funcs.json`;
`build/scratch/system/sdkcalls.txt` lists every caller).

### 3.1 Threads and semaphores (implement natively, EE semantics)

| Function | Sites | Native semantics |
|---|---|---|
| `CreateThread(ThreadParam*)` | 7 | new DORMANT thread; uses `entry`, `initPriority`, `stackSize` (as a hint; `stack` and `gpReg` ignored); returns id >= 1 |
| `StartThread(tid, arg)` | 7 | DORMANT -> READY, entry(arg); returns tid; switch if more urgent |
| `ExitDeleteThread()` | 6 | end the calling thread, free its id |
| `TerminateThread(tid)`, `DeleteThread(tid)` | 2 each | (usbd, mpeg) stop another thread; see 1.7 |
| `ChangeThreadPriority(tid, prio)` | 11 | returns the **previous** priority; reschedule |
| `RotateThreadReadyQueue(prio)` | 1 | move the head of that ready queue to its tail |
| `GetThreadId()` | 22 | current game thread id |
| `ReferThreadStatus(tid, p)` | 1 | fill `currentPriority` (+0x18) at least |
| `SleepThread()` | 14 | consume a wakeup or block |
| `WakeupThread(tid)` | 10 | ready it, or count the wakeup; switch if more urgent |
| `iWakeupThread(tid)` | 7 | same from an interrupt (also for the interrupted thread itself); deferred switch |
| `CancelWakeupThread(tid)` | 3 | clear the count, return it |
| `CreateSema(SemaParam*)` | 12 | uses `initCount`, `maxCount`; id >= 1 |
| `DeleteSema(sid)` | 17 | waiters are released with an error result |
| `WaitSema(sid)` | 130 | returns sid; blocks at 0 |
| `PollSema(sid)` / `iPollSema` | 4 / 1 | returns sid if taken, else a negative code |
| `SignalSema(sid)` | 31 | count + 1 up to max (a full semaphore stays full and returns an error); switch if a more urgent waiter was released |
| `iSignalSema(sid)` | 2 (one is `signal_sema_helper`, 43 callers) | same, deferred switch, also from thread context |

### 3.2 Interrupts and cache (implement or no-op)

| Function | Sites | Native |
|---|---|---|
| `AddIntcHandler` / `AddIntcHandler2` | 3 / 3 | register with the platform interrupt thread (VBLANK_S, VBLANK_E; GS, VIF1, IPU only if the graphics / movie rewrites raise them) |
| `RemoveIntcHandler` | 5 | unregister |
| `AddDmacHandler` / `AddDmacHandler2` / `RemoveDmacHandler` | 1 / 2 / 1 | same for the DMA channels the rewrites keep |
| `EnableIntc` / `DisableIntc` / `EnableDmac` / `iDisableIntc` | 5 / 4 / 1 / 2 | per-cause mask |
| `intr_suspend` (+ inline `intr_resume`), `DIntr` / `EIntr`, `DI()` / `EI()`, `__asm__("ei")` | 124 / 25 + 25 / 52 | the recursive interrupt lock of 1.7; `EIntr` enables unconditionally and returns the previous state |
| `FlushCache(0)` | 11 | no-op |
| `SyncDCache(start, end)` | 8 | no-op |
| `wait_vblank_start` | 3 | not called (USB / i.LINK stubs) |
| COP0 `mfc0` / `mfpc` / `mtpc` / `mtps` | see 2.5 | counters |

### 3.3 Output, start-up, exit

| Function | Native |
|---|---|
| `printf_to_deci2` -> `sce_printf_deci2("%s", s)` (`src/engine/pdi/pdistd_misc.c:94`), the back end of pdistd `printf` (52 game, 12 engine call sites) | append to the log (file + debugger + console), thread-safe |
| `_InitSys`, `InitMainThread`, `InitHeap`, `__main` (`T000_main.cpp:111`) | platform start-up (8.5), run the game's static constructors first (runtime.md) |
| `Exit`, `kill`, `abort`, `_exit` | not reached (main loops forever); abort -> log + break |
| `ExecPS2`, `LoadExecPS2`, `ResetEE`, `SetGsCrt`, `GsPutIMR`, `GetOsdConfigParam(2)` (libscf only), `SetAlarm`, `Deci2Call` | not called by game / engine (graphics owns the GS ones) |
| `InitHeap`, `EndOfHeap`, `sbrk` | the newlib `_malloc_r` heap, which nothing uses (the game's heap is pdistd's, 8.2): not needed |

Used only inside `src/sdk` (dropped with it): `SetAlarm` / `iSetAlarm` /
`iReleaseAlarm` / `InitAlarm` (libmc, libcdvd delays), `SuspendThread`,
`iSuspendThread`, `iRotateThreadReadyQueue`, `_iWakeupThread`, `InitThread`
(the top thread), `InvalidDCache` / `iSyncDCache`, the `_i`-forms of the
INTC / DMAC masks, `SetVSyncFlag`, `SetGsCrt` / `GsPutIMR` /
`sceGsGetGParam` (libgraph), `GetOsdConfigParam(2)` (libscf), `Deci2Call`
and the `sceDeci2*` TTY, `Exit`, and the newlib syscall glue (`write`,
`lseek`, `fstat`, `isatty`, `getpid`, `sbrk`, `__errno`).

### 3.4 IOP plumbing (remove)

| Function | Sites | Callers |
|---|---|---|
| `sceSifInitRpc` | 1 | `IOPLoadModuleBase::cmdInit` (`T524:101`) |
| `sceSifBindRpc` | 3 | `pdi_bind_rpc` (`T527:14`), `pjoy_bind`, `pdi1394_bind` |
| `sceSifCallRpc` (via `sif_call_rpc`, `T528:11`) | 5 | `PDIClientRpc::call`, pjoy, pdi1394 |
| `sceSifCheckStatRpc` | 6 | pjoy, pdi1394 |
| `sceSifSetRpcQueue` / `sceSifRegisterRpc` / `sceSifRpcLoop` | 2 / 2 / 2 | usbd and pdi1394 EE servers |
| `sceSifRebootIop`, `sceSifSyncIop`, `sceSifLoadFileReset`, `sceFsReset`, `sceSifInitIopHeap`, `sceSifLoadModule` | 1 each | the IOP script (`T524`) |
| `sceCdInit`, `sceCdMmode` | 1 each | `cmdInit` |
| `sceOpen` / `sceClose` / `sceLseek` / `sceRead` / `sceWrite` | 1 each | the `host:` wrappers `host_open..host_close` (`src/engine/io/hostfs_io.c`) |

### 3.5 Device libraries (replace with native implementations)

- libpad (13 functions + `pad_rpc_cmd_0x14`): section 5.
- libmc (17 functions, all through `mcard_io.c`): section 6.
- libscf (`sceScfGetGMTfromRTC`, `sceScfGetLocalTimefromRTC`,
  `sceScfGetAspect` from the enhanced engine's `scf_get_aspect`,
  `T512_render_context.cpp:155`): section 7.1.
- libgraph `sceGsResetGraph` (`gfx_init`): graphics.

## 4. File I/O

### 4.1 Layers

```
 game: file_read_blocking / file_read_sound_bank / file_req_alloc+file_req_run /
       AsyncLoader (T507) -> loader thread (T508) / file_locate (BGM) /
       file_resolve_retry (movies) / host_read_file (T178 debug dumps)
                    |
 T502 file_resolve, file_locate  -- T500 TOC lookup ("RoFS", in memory)
                    |                T510 VolDevice / vol_mount (TOC read once)
 T503 vol reader thread (one request at a time, waiters, cancel)
      T511 GZIP::InflateLoader (T537 inflate) for packed files
                    |
 T523 pdipipe (RPC 'PDIP': open lsn/size or host file, read, read to SPU, close)
 T519 pdicdvd (RPC 'PCDV': sector reads; 'Pcdv': clock)  <- T525 sector cache <- T526 ISO 9660
 hostfs_io.c host_open.. (sceOpen through the global lock)
                    |
 IOP: pdipipe / pdistr / pdicdvd modules, cdvdman, fileio, host:
```

Requests: `file_read_blocking(path, buf, size, unpack, dev)`
(`T503:329`) registers a `FileWaiter`, signals the reader and sleeps. A
NULL `buf` is `memalign(64, size rounded up to 64)`. `unpack` inflates a
packed (`.gz` in the TOC, type 2) file. The reader reads the stored bytes
in 0x40000-byte `pdipipe_read` pieces (cancel checked between pieces) or
inflates them with a 0x2000-byte window. Sound banks bypass the thread
(`file_read_sound_bank`, `T503:372`, on the caller's thread): header to EE
memory, samples straight to SPU2 memory with `pdipipe_read2(..., toSpu=1)`
where `buf` is an SPU address.

### 4.2 Keep or replace

| Layer | Files | Decision |
|---|---|---|
| TOC decode / lookup | `src/engine/io/T500_vol_toc.cpp` | keep |
| `VolDevice`, `vol_mount` | `T510_vol_device.cpp` | keep; its `iso9660_find` call gets the native record (4.3) |
| `file_resolve`, `file_locate`, `file_size_retry` | `T502_file_resolve.cpp` | keep; remove the R1 spins |
| reader thread, requests, waiters | `T503_vol_thread.cpp` | keep (the thread is how the menus load in the background); replace the VLA (runtime.md) |
| queued loads | `T504_file_queue.cpp` | keep |
| image packs | `T506_image_pack.cpp` | keep |
| AsyncLoader, loader thread | `T507`, `T508` | keep |
| Serialize | `T509` | keep |
| InflateLoader | `T511` | keep; its input is the native pipe |
| inflate, crc32 | `src/engine/pdistd/T537_gzip_inflate.cpp` | keep |
| `pdicdvd_read`, `pdicdvd_read_start/wait`, `pdicdvd_get_time`, `pdicdvd_set_disc_id`, `F_0025B620` | `src/engine/pdi/T519_pdicdvd.cpp` | **replace**: Win32 read of gt3.vol at `lsn * 2048` (64-bit offset), return 1; native clock; the rest no-ops. Keep `pdicdvd_time_to_clock*`. |
| `pdipipe_open/read/read2/close` | `T523_pdipipe.cpp` | **replace** (4.3) |
| sector cache, ISO 9660 | `src/engine/io/T525_sector_cache.cpp`, `T526_iso9660.cpp` | drop (only `iso9660_find` is still called, by `vol_mount` and `file_resolve` type 1) |
| RPC base | `T527_pdi_client_rpc.cpp`, `T528_sif_call_rpc.cpp` | drop |
| `host_open..host_close`, `io_getstat` | `src/engine/io/hostfs_io.c` | **replace** with Win32 (later pass: loose files) |
| `host_read_file` / `host_write_file` / `host_file_size` / `host_file_exists` | `T505_host_files.cpp` | keep (they call `host_*`) |

### 4.3 Native volume

Pass 1's design, below. Since the "game directory files" pass (roadmap
item 1) the game no longer opens `gt3.vol`: `vol_init` mounts the original's
loose `host:data/` device (type 3), whose files the launcher unpacks into
`data\`; `vol_mount` lists that folder as a TOC (`host_file_toc`) for the
course and car lists, every read goes through `pdipipe` / `pdibgm` /
`mpg_service`'s `host:` branch, and the sector paths below are left unused
(`platform/disc.h`; `tools/pssdump` still reads gt3.vol with them).

- At start-up open `gt3.vol` from the game folder (`gt3_disc/files/gt3.vol`
  during development) once for shared reads. It is 3,876,222,976 bytes:
  sector offsets reach 1,892,687 and **byte offsets exceed 2^31**, so every
  native offset is `u64`. (The PS2 code's host-volume path,
  `vol_open_name` type 2, does `lsn <<= 11` in an `int`: it would overflow,
  but GT3 never mounts that type.)
- `iso9660_find("gt3.vol")` returns a static `IsoDirRecord` with `lsn` 0 and
  `size` = the file size in both byte orders (`iso_read_u32` reads them);
  `vol_mount` then reads the TOC header and body through `pdicdvd_read`
  unchanged, with `extent.lsn = 0`. Every LSN the game computes (TOC lsn +
  `extent.lsn`) is then a sector of gt3.vol. Any other name fails (log it).
- `pdicdvd_read(buf, lsn, size, mode)`: positional read (`ReadFile` with an
  OVERLAPPED offset), so several threads can read at once without sharing a
  file pointer; returns 1, 0 on error.
- `pdipipe`: a handle table, each `{u64 base, u64 pos, u32 size, HANDLE}`.
  `open(lsn, size, bufSize, hostPath)`: `hostPath` empty -> gt3.vol at
  `lsn * 2048`; non-empty -> a `host:` file (later pass) at byte offset
  `lsn` (0 for loose files). `read(h, buf, n)`: read at `base + pos`,
  `pos += n` (short read past the end: zero-fill and log). `read2(h, buf, n,
  toSpu)`: toSpu 0 as `read`; toSpu 1 -> read into a temporary and hand it
  to the native SPU2 RAM at address `(u32)buf` (sound agent's API).
  `close`. The vol thread and `file_read_sound_bank` use the pipe at the
  same time: handles are independent.
- Each native read is a scheduler switch point (1.7).

### 4.4 Streaming paths (other agents)

- BGM: `ads_play` (`src/game/sound/T466_bgm_ads.cpp:106`) resolves
  `bgm/<name>` with `file_locate` and calls `pdibgm_start(lsn, sectors,
  arg, 0x40000, hostName)`: lsn is a gt3.vol sector natively. A missing
  file plays SE 4 and returns (graceful).
- Movies: `mpegif::preload` / `startThread` (`T529_mpegif.cpp:103`,
  `:199`) use `file_resolve_retry` (R1: spins on a missing movie) and
  `pdimpeg_open(1, lsn, sectors, host)`; the feeder reads 32 KB blocks.
- Sound banks: `file_read_sound_bank` (above), `GTSOUNDINSTRUMENT::loadFile`
  (`T468_gtsound.cpp:216`).

The platform should expose one primitive for them: read N bytes of gt3.vol
at a sector, positional and thread-safe.

### 4.5 Error and retry paths

- Missing file: R1 (5 spins), R2 (`vol_mount`). BGM is the only caller that
  handles -1.
- `file_request` while the reader is busy returns 0 and
  `file_read_blocking` waits for the running request, then retries; a buffer
  it allocated for the refused request leaks (original behaviour).
- Cancel: `file_req_cancel` (`T503:314`, from `loader_cancel`) stops a read
  between 0x40000 pieces; an allocated buffer is freed and the result is
  NULL.
- `pdicdvd_read` failure made `sector_cache_read` return NULL and
  `iso9660_find` send `F_0025B620` (pdicdvd fno 1): gone natively.
- `host_read_file` returns NULL when the size read differs (a malloc'ed
  buffer leaks).
- `io_getstat` (`hostfs_io.c`) always reports mode 0x11, so host
  directories are never recognised (`file_resolve` type 3).

### 4.6 host: and the loose-file pass

The `host:` logic the later "game directory files" pass will reuse:

- `VolDevice::type` 3 (`VOL_DEV_HOST_FILES`, `include/engine/io/T510_vol_device.h:23`):
  `file_resolve` (`T502:81`) probes `host:data/<path>` and
  `host:data/<path>.gz` (packed); `vol_open_name` (`T503:88`) and
  `file_locate` (`T502:154`) build the same names for the pipe and for BGM /
  movies.
- Type 2 (a `host:` copy of the volume): `vol_mount` (`T510:115`) through
  `host_file_size` / `host_read_file`; `vol_open_name` :82.
- `T505_host_files.cpp`: `"host:" + path` helpers; the only game user is
  the course environment dump `T178_course_env.cpp:351`, `:359` (debug).
- `hostfs_io.c`: `host_open` / `host_lseek` / `host_read` / `host_write` /
  `host_close` / `io_getstat` over `sceOpen..` under `global_lock_acquire`.

Natively `host:` maps to the game folder; pass 1 mounts type 0 only.

## 5. Controllers

### 5.1 libpad as the game uses it

`PadManager::init` (`src/engine/input/T475_pad_manager.cpp:40`) ->
`padStart` (`T477_ps_port.cpp:261`): `scePadInit(0)`,
`pad_rpc_cmd_0x14(0)`, `scePadPortOpen(port, 0, dmaArea)` for pad indices 0
and 4 (port 1 and port 2, slot 0; `PAD_STEP` 4: no multitap), the pad
thread.

Per frame (render thread, `PadManager::update`, `T475:68`): `padReadAll`
(`scePadRead` of both ports into `PadState::data`), each `PSPort::update`
(mode id + data), `padWakeThread`, the USB ports, then every attached
`Control` under the PadManager semaphore (`Control::update`: `latch`,
`port->translate`, `emulatePressure`).

The pad thread (`padThreadMain`, `T477:93`) per port:
`scePadRead` into `rdata`; `scePadGetState`: Discon / Error reset the
state machines; FindCTP1 / Stable continue:

1. main mode: `scePadInfoMode(InfoModeIdTable, -1)` = mode count,
   `InfoModeCurID`, `InfoModeCurExID`; if the table has type 7 (DualShock)
   and the pad is not in it, `scePadSetMainMode(offs, lock 3)`, then
   `scePadGetReqState` until Complete (Faild retries);
2. pressure mode: if `rdata[1] != 0x79` and `scePadInfoPressMode == 1`,
   `scePadEnterPressMode`, then `scePadGetReqState` until Complete;
3. actuators: `scePadInfoAct(-1, 0)` = count; `scePadSetActAlign({0, 1,
   0xFF..})` once; then `scePadSetActDirect` whenever the motor bytes
   change (`{small (0/1), large (0..255)}`; a one-motor pad gets
   `{small or large >= 0x61, 0x40}`).
4. `stable = 1` once FindCTP1 / Stable.

`scePadEnd` only from `padStop` (no caller).

### 5.2 Data layout PSPort and Control read

`PadData` (0x20 bytes, `include/engine/input/T477_ps_port.h`):

| Byte | Content |
|---|---|
| 0 | 0 = valid data (0xFF when the read failed) |
| 1 | mode id: 0x41 digital, 0x73 DualShock (analog), 0x79 DualShock 2 (analog + pressure); `padGetData` returns it only once `stable` |
| 2, 3 | buttons, active low: `raw = ((b[2] << 8) \| b[3]) ^ 0xFFFF` |
| 4, 5 | right stick X, Y (0..255, 0x80 centre, Y down positive) |
| 6, 7 | left stick X, Y |
| 8..19 | pressures 0..255: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 |
| 20..31 | unused |

`raw` bits (`include/libpad.h`, `PADRAW_*` in `T474_control.h`): L2 0x0001,
R2 0x0002, L1 0x0004, R1 0x0008, triangle 0x0010, circle 0x0020, cross
0x0040, square 0x0080, select 0x0100, L3 0x0200, R3 0x0400, start 0x0800,
up 0x1000, right 0x2000, down 0x4000, left 0x8000.

`PSPort::translate` (`T477:415`) into `Control`
(`include/engine/input/T474_control.h`): `type` = PS type from the mode id
(0x41 -> 4, 0x73 -> 7, 0x79 -> 8; unknown ids clear everything);
`channels[i] = data[4 + i]` for i = 0..31 (ch 0..3 sticks, 4..15 pressures;
28..31 read past the data into `actEnable`, harmless);
`curButtons.raw[0] = raw`; `curButtons.buttons` = GT3 buttons: up 1, down
2, left 4, right 8 (d-pad, or the left stick through the curve
`g_003416F8` beyond +-0x41 of centre), start 0x10, select 0x20, decide 0x40
(circle or cross), cancel 0x80 (triangle or square).
`Control::emulatePressure` (`T474_control.cpp:108`) gives pads of type 4
and 7 pressures of 0x80 for held buttons.

Users: `MenuControl` (`T214`) reads raw bits L1..R3, the d-pad pressures
and `CHAN_RSTICK_Y`; `RaceInput` (`T413`) reads raw bits, the d-pad
pressures for pressure steering (`readPressureSteer`, full at 60 %), the
stick channels through `ControllerConfig` (`T426`: per function a key code
`{raw button, axis channel, pressure channel}`, defaults: steer = left
stick, throttle = cross, brake = square, circle, triangle, R1, L1, R2, L2,
start).

### 5.3 USB wheels and i.LINK

- USB (`src/engine/input/usbd.c`, `pjoy.c`, `T479_usb_steering_port.cpp`,
  `T478_steering_ps_port.cpp`): `usbd_init(1)` from `cmdInitClients`
  (`T009:145`) starts two priority-1 threads and a VBLANK_S handler;
  `usbjoy_start` (`T009:153`) binds 'PJOY'. Stub: no threads, no handler.
  As built (`src/engine/input/usb_pc.cpp`, `platform/wheel.cpp`): PJOY
  ports 0 / 1 are the PC's Wheel 1 / 2, each a Logitech Driving Force
  (0x46D:0xC294: 10-bit wheel, pedals at 0xFF released, 12 buttons, hat)
  mapped from an SDL joystick (gt3pc.ini `[Wheel1]` / `[Wheel2]`,
  `platform/input_map.h`); without one `pjoy_read_state` returns
  `PJOY_STATE_REMOVED` (3) and the `USBSteeringPort` reports no device.
  Force feedback (`ff_*`): accepted and dropped while a wheel is
  connected (T479's spring set-up and auto-centring go on; the centring
  waits for the wheel near its centre). `SteeringPSPort` does not merge a
  local player's pad and wheel: with both connected its port is the device
  `[WheelN] Input` chooses (the wheel by default; the other ignored, its
  actuators and menu steering too). Merged, the race steered with the wheel
  whenever the pad's steering was smaller, so a wheel resting off its
  centre steered while the stick was inside its dead zone (user report).
  The wheel's pedals are linear over their travel (`raceinput_wheelPedal`).
- i.LINK (`src/engine/pdi/pdi1394_rpc.c`, `T541_pdi1394.cpp`,
  `src/engine/system/T473_link_session.cpp`, `T480_ieee1394_port.cpp`):
  `pdi1394_init(1)` starts a priority-1 server thread; `pdi1394_bind`;
  `F_0022D320` (game_init) creates the session object. Stub: status block
  with no nodes (own slot 0, group counts 0) so the arcade "i.LINK" entry
  shows no partners; `pdi1394_exchange` fails; no threads.

### 5.4 Vibration

`Control::setActuator` -> `PSPort::setActuator` (`act[0]` small, `act[1]`
large) -> `padSetActuators` each frame (`PSPort::setPadId`) -> pad thread
`scePadSetActDirect`. Game side: `T335_pad_vibration.cpp`, the options'
vibration flag per player.

### 5.5 Native pad backend

Implement the libpad entry points the game calls (keeps T475 / T477
unchanged) over a native device per port:

| Function | Native result |
|---|---|
| `scePadInit`, `pad_rpc_cmd_0x14`, `scePadPortOpen`, `scePadEnd` | 1 / ignore the DMA area |
| `scePadRead(port, 0, buf)` | fill the 0x20-byte buffer (5.2) from the device snapshot taken at the last vblank; return 32; 0 when the port has no device |
| `scePadGetState` | 6 (Stable) with a device, 0 (Discon) without |
| `scePadInfoMode` | `(InfoModeIdTable, -1)` = 2 modes; `(InfoModeIdTable, 0)` = 4, `(InfoModeIdTable, 1)` = 7; `InfoModeCurID` = 7 after `SetMainMode` (4 before); `InfoModeCurExID` = 0 |
| `scePadSetMainMode`, `scePadInfoPressMode`, `scePadEnterPressMode` | 1 (after `EnterPressMode` the mode id is 0x79); `scePadGetReqState` = 0 (Complete) |
| `scePadInfoAct`, `scePadSetActAlign`, `scePadSetActDirect` | `InfoAct(-1, 0)` = 2 motors; `SetActAlign` = 1; `SetActDirect(port, 0, d)` = 1 and drives XInput rumble (`d[0]` small on/off, `d[1]` large 0..255) when the device has motors |

Simpler and equivalent: report type 7 and mode id 0x79 from the start;
the pad thread's state machines then finish on the first frame.

Keyboard first (port 1), a DualShock 2 with full pressure on held keys:

| Pad | Keys (proposal) | Bytes |
|---|---|---|
| d-pad | arrow keys | bits + pressure 0xFF |
| left stick | arrows also move it to 0x00 / 0xFF (menus use the d-pad, racing the stick); centre 0x80 | b[6], b[7] |
| right stick | I / J / K / L | b[4], b[5] |
| cross / square / circle / triangle | X / Z / C / S | bits + pressures 0xFF |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 | bits + pressures |
| start / select | Enter / Backspace | bits |
| L3 / R3 | F / H | bits |

XInput (pass 1, port 1 or 2 when connected): sticks scaled to 0..255,
triggers as L2 / R2 pressure, buttons as 0xFF pressure. Mappings belong
in a config file later (roadmap: controller passes).

As built (roadmap "Controller support 1"): the controllers come through
SDL3 instead of XInput (`platform/gamepad.cpp`, a DualShock 4 first, any
SDL gamepad works); port 1 is the keyboard and the first controller
combined, port 2 the second controller. The answers are built through the
mappings of gt3pc.ini `[Controller1]` / `[Controller2]`
(`platform/input_map.h`: per DualShock 2 control a gamepad slot and a
keyboard / mouse slot, the sticks' and triggers' dead zones; edited by the
launcher's Controls tab), the default keys those of `platform/pad_pc.cpp`'s
header. The menus (F2, Esc) read the physical buttons; a controller's
Guide button opens them (a tap the settings menu, a 1.5 s hold the Esc
menu).

Reads and the dynamic frame rate (`platform/pad_pc.h`; renderer.md "Frame
rate"): when the frames are slower than the ticks, one frame runs several
ticks back to back. Their reads would fall within microseconds of each
other, so a press showed on the first tick only, and code that looks for a
press later in the frame (the draw, the post-draw slot) missed it (user
report: inputs dropped below 60 frames a second). Those ticks now repeat
the last read (`padReadAll` returns at once while `pad_pc::holding()`; the
render loop sets it for all but the frame's last tick), so the last tick
reads the pads and a press shows once, there. And every read reports the
buttons pressed since the last read as pressed (`pad_pc::take_taps`: the
keyboard's key-down messages, the SDL thread's polls), so a tap shorter
than the time between two reads still counts.

## 6. Memory card

### 6.1 Layers

```
 game: memory card screens (T068..T078, T110), MemorycardFileManager threads (T109),
       file classes and save names (T108), boot warning (T078), autoload (T075)
        |  card id = port * 4 + slot; status MC_STATUS_* (T501_memorycard_manager.h:53)
 T501 memory-card manager: slot state polls (type, free, formatted, changed),
      blocking operations under lock_sema, worker thread (polls + async requests)
        |
 mcard_io.c: mc_*_sync = WaitSema(lock); while (sceMcXxx() != 0) yield; mc_sync_wait; unlock
        |
 libmc -> mcserv / mcman (IOP)
```

### 6.2 Operations and results the menus expect

libmc calls (all through `mcard_io.c`): `sceMcInit`, `GetSlotMax`,
`GetInfo(type, free, format)`, `Open`, `Close`, `Seek`, `Read`, `Write`,
`Mkdir`, `GetDir`, `Delete`, `Format`, `Unformat`, `Rename`,
`SetFileInfo`, `GetEntSpace`, `Sync`.

- **Slots**: `nslots = min(4, GetSlotMax(port))`; natively 1 per port: card
  ids 0 (port 1) and 4 (port 2).
- **GetInfo** result 0 (`type` 2 = PS2, `free` in 1 KB clusters, `format` 1
  formatted / 0 not); -1 (`sceMcResChangedCard`: the manager asks again with
  free / format and flags `changed`); -2 (`NoFormat`); -11 or lower (no card: the manager
  clamps at `MC_RES_NO_CARD`, `src/engine/io/vol_internal.h:99`). `type` 0 = no card. Polled one slot every 4th frame by
  the worker and on demand by `mc_slot_refresh`.
- **Open** flags: `SCE_RDONLY` 1, `SCE_WRONLY` 2, `SCE_RDWR` 3, `SCE_CREAT`
  0x200, `SCE_TRUNC` 0x400, and 0x100 (`SCE_APPEND`, used with seek-then-
  write: treat it as "no effect"). `mc_do_create_file`: 0x702;
  `mc_do_write_file`: 0x103, and when that gives -4 (no entry) at offset 0,
  `SCE_CREAT` alone (0x200). Returns fd >= 0 or a result code.
- **Read / Write** return bytes; **Seek**(fd, off, SET) returns the new
  position (compared with `offset`); **Close** 0.
- **Mkdir** 0; existing -> error. **Delete** 0 (files; empty directories).
  **Rename**(old, new) within the root (`saveFiles`, `T108:1463`).
  **SetFileInfo** with `sceMcFileInfoAttr` only (`copyTo`, `T108:478`; the
  info is a `McDirEntry`, attr at +0x14 as in `sceMcTblGetDir`).
  **Format** >= 0; **Unformat** >= 0 (no game caller).
- **GetDir**(port, slot, pattern, mode, maxent, table): mode 0 starts a
  listing, 1 continues it; returns the number of 0x40-byte `sceMcTblGetDir`
  entries written (`include/libmc.h:106`: create / modify date
  {resv, sec, min, hour, day, month, year}, size, attr, name[32]). Patterns
  used: `/BASCUS-97102*`, `/BASCUS-97102<type>*` (`mcMakeSearchPattern`,
  `T108:1243`), `/<dir>/*` (the files of a save, with `.` and `..`),
  `/<dir>` and `/<dir>/<file>` (one entry: existence and size). `*` and `?`
  wildcards.
- **Attributes** the game tests: 0x1000 PS1 save, 0x0008 copy prohibited,
  0x0020 directory; write directories as 0x8427 and files as 0x8497.
- **Result codes** (`include/libmc.h:56`): 0, -1 changed card, -2 not
  formatted, -3 full, -4 no entry, -5 denied, -6 not empty, -7 too many
  handles, -8 replace failed. `mc_status_from_result` maps -4 to
  `MC_STATUS_NO_ENTRY` and -1 / -2 / <= -11 to `MC_STATUS_CARD_CHANGED`.
- **Free space**: the boot warning (`warning_check_card`, `T078:93`) wants a
  PS2 card with `free - 2 >= needKB` (or unformatted, or the game data
  already there); `MemorycardGT3File::calcSize` (`T108:1434`) counts 1 KB
  clusters: data, icon.sys and thumbnail one each, the icons, and the
  directory entries two per cluster.

### 6.3 What GT3 writes

Directory names: `BASCUS-97102` (`mcGetNamePrefix`, from `"BA"` +
`"SCUS-97102"`, `T009:30..31`) + a type letter + 7 base-64 digits
(`A-Za-z0-9+-`, low digit first) of a 42-bit code (car, course, category,
subcategory, 12-bit unique number); the game data is the fixed
`BASCUS-97102GAMEDATA`. Types: `G` game data, `R` replay, `B` best-lap /
licence replay, `S` car setting.

In each save directory `/<NAME>/`:

| File | Size | Written by |
|---|---|---|
| `<NAME>` (the data, named like its directory) | the serialised data (`Serialize`, T509) | `mc_create_file` in `saveFiles` (`T108:1463`) |
| `thumbnail` | `sizeof(GameThumbnail)` 0x60 | `saveFiles`, `changeComment` |
| `icon.sys` | 0x3C4 (`sceMcIconSys`, title in Shift-JIS) | `writeIconSys` (`T108:810`), template `menu/icon/icon.sys` from the volume |
| `icon0.ico`.. (`icon%d.ico`) | the `.ico` from the volume: `menu/icon/ICON_a_spec.ico` (G), `ICON_full.ico` (R), `ICON_best.ico` / `ICON_license.ico` (B), `ICON_setting.ico` (S) | `writeIcons` (`T108:835`) |

Other games' saves are listed and shown (icon.sys read, 0x1C4 bytes;
icons), copied (`copyTo`) and deleted.

### 6.4 Native memory card on files

Replace the 17 `mc_*_sync` wrappers of `src/engine/io/mcard_io.c` (the only
libmc callers) with synchronous native versions; T501 and the game stay
as they are.

- Card N = folder `memcard/card<N>/` in the game folder (N = 1, 2; roadmap:
  "two folders that stand for memory cards 1 and 2"). Folder present = a
  formatted PS2 card (`type` 2, `format` 1); absent = no card (`type` 0,
  result -11); a folder with a marker file `unformatted` = unformatted card
  (result -2, `type` 2, `format` 0). `Format` = empty the folder and remove
  the marker; `Unformat` = empty it and add the marker.
- Capacity 8000 KB (configurable). `free` = capacity - used, with used =
  sum over files of ceil(size / 1024) + per directory ceil(entries / 2)
  (entries counting `.` and `..`), the same model as `calcSize`.
- Report -1 (changed) once after a card folder appears or disappears while
  running (so the menus refresh); 0 otherwise.
- Files map 1:1 to host files (names as written; the PS2 names are ASCII,
  at most 31 characters). File descriptors: a small table, at most 16 open.
  Writes go to a temporary file renamed over the target on `Close`, so a
  quit in the middle of a save never leaves a torn file.
- `GetDir` entries: dates from the host file times converted to JST (the
  PS2 card stores the RTC's time), attributes as in 6.2, listing order =
  creation order (the game sorts itself, `T109`), `.` and `..` first in a
  directory listing; `mode` 1 continues from the previous position of the
  same pattern.
- No PS1 saves are produced.
- `GetSlotMax` = 1, `GetEntSpace` = free clusters (no game caller), `Init`
  once.

## 7. System configuration and boot

### 7.1 libscf

`src/sdk/libscf/libscf.c` reads the OSD configuration
(`GetOsdConfigParam`: spdif bit 0, aspect bits 1..2, Japanese-language bit
4, version bits 13..15, language bits 16..20, time zone bits 21..31 signed
minutes; `GetOsdConfigParam2` byte 1: date notation, summer time, time
notation) and `rom0:ROMVER` (T10K detection) through `sceOpen`.

GT3 uses:

- `sceScfGetGMTfromRTC`, `sceScfGetLocalTimefromRTC` (2.4) for the clock
  dates;
- `sceScfGetAspect` through the enhanced engine's `scf_get_aspect`
  (`T512_render_context.cpp:155`, `T428_game_options.cpp:52`): 2 = 16:9
  sets the render context's `wide` flag.

It does not use the console language: the country / language is the
build's (`Country` = `COUNTRY_US`, `src/game/data/T516_country.cpp:13`,
then the saved `GameStatus::language` via `set_country`).

Native: keep the clock arithmetic of `libscf.c`; implement the getters
(drop `scf_get_romver` / T10K): aspect 0 (4:3) in pass 1, language 1
(English), time zone = Windows' bias in minutes (`GetTimeZoneInformation`,
sign flipped), summer time = Windows' daylight flag, date notation 0, time
notation 0.

### 7.2 Boot arguments

`main(argc, argv)` -> `game_boot` (`src/game/app/T007_game_init.cpp:137`):
`hot = argc >= 2 && argv[1] == "hot"`. The bootstrap (SCUS_971.02) starts
CORE.GT3 with `{path, "hot"}`: then `gfx_init(1, 1, 2, 0)` keeps the display
and the render thread first fades the bootstrap's image at VRAM 0x50000
(`boot_screen_fade`, `T002:107`). Natively there is no bootstrap: call
`game_boot(1, {"gt3pc"})`, the cold path (`gfx_init_default(2)`, no fade).

### 7.3 IOP modules and RPC servers

`RaceIOPLoadModules::load` (`src/game/app/T009_race_iop_load_modules.cpp:128`)
runs the script `g_00308B00` through `IOPLoadModuleBase::run`
(`src/engine/pdi/T524_iop_load_module_base.cpp:73`): media mode
(`sceCdMmode(2)`, DVD), reboot the IOP with `cdrom0:\IOPRP224.IMG;1`, then
from `cdrom0:\...;1`:

| Module | Purpose | Native |
|---|---|---|
| ioprp224.img | IOP kernel modules (cdvdman, fileio...) | none |
| sio2man, padman, mtapman | pads (servers 0x80000100 / 0x80000101) | native libpad (5.5) |
| mcman, mcserv | memory card (server 0x80000400) | native card (6.4) |
| libsd, pdispu2 | SPU2 sound ('SPUP', 'SPUV') | native mixer (sound agent) |
| usbd, pdiusb | USB wheels ('PJOY'; EE server 'PUSB') | stub |
| ilink, pdi1394 | i.LINK ('PFIM', 'PFIS'; EE server 'PFWE') | stub |
| memclear | clears IOP memory | none |
| libpdi | base of the pdi modules | none |
| pdicdvd | sector reads ('PCDV'), clock ('Pcdv') | native volume + clock (4.3, 2.4) |
| pdistr | "PDI_Streaming_service" (imports pdicdvd and pdispu2): the streams behind the EE clients 'PDIP' (pipe), 'PBGM' (BGM), 'MPG1' / 'MPG2' (MPEG) | native pipe (4.3); BGM and MPEG streams: sound / movie agents |

`cmdInitClients` (`T009:143`) runs between the reboot and the pdi modules:
`usbd_init(1)`, `pdi1394_init(1)`; `cmdBindClients` (`T009:151`) after
them: `usbjoy_start`, `pdi1394_bind`, `pdicdvd_init`, `pdispu2_init`,
`pdibgm_init`, `pdimpeg_init`, `pdipipe_init`. Sony's own servers
(cdvdfsv 0x80000592..0x8000059A, fileio 0x80000001, loadfile 0x80000006,
iopheap 0x80000003) go with the SDK libraries.

Natively `RaceIOPLoadModules::load` becomes a plain function that calls the
native inits (open gt3.vol, sound device, MPEG, memory cards); the script
tables, `IOPLoadModuleBase` and its member-pointer table (runtime.md) are
not compiled.

### 7.4 system.cnf

`BOOT2 = cdrom0:\SCUS_971.02;1`, `VER = 1.00`, `VMODE = NTSC`: the
bootstrap, NTSC (59.94 Hz, 640 x 448 interlaced). The port starts the core
directly; NTSC timing is the vblank rate of 2.3.

### 7.5 `game_init` step by step

`game_init(iop, hot)` (`T007_game_init.cpp:100`), main thread at priority 1:

| # | Step | PS2 | Native |
|---|---|---|---|
| 1 | `scf_lock_init` | semaphore | keep |
| 2 | `gfx_init_default(2)` (cold) | GS reset, VU microcode, INTC / DMAC handlers, scratchpad context | graphics; registers the vblank handler with the platform |
| 3 | `g_002F4230.init()` | render context, VRAM buffers | graphics |
| 4 | `iop->load()` | IOP script (7.3) | native inits; no threads for USB / i.LINK |
| 5 | `vol_init("gt3.vol")` | sector cache, mount (TOC read), `mc_manager_init` (8 card polls), reader thread | same over the native volume and card |
| 6 | `realtime_load` | texts | file reads |
| 7 | `g_002EA7F8.init()` | PadManager: libpad, pad thread | native libpad |
| 8 | system font | `font/system.fnt`, `font/system.kf` | file reads |
| 9 | `sound_init` | sound banks to SPU, music thread, VBLANK_E handler | sound agent |
| 10 | car, colour and race-mode databases, `car_list_load`, `course_table_init` | files + `qsort` | keep (newlib qsort, runtime.md) |
| 11 | `race_pool_assign(malloc(0x22000))` | race object pool | keep (PC: 1 MB, ROADMAP 18) |
| 12 | `F_0022D320` | i.LINK session object, its Control attached | stub link |
| 13 | `loader_init`, `product_config_init`, `menu_car_spec_create`, `createGameStatus`, `showroom_load`, GameStatusHolder | files, heap | keep |
| 14 | `render_thread_release` | `render_thread_start(0)` (main -> 9), render semaphore signalled | keep |

Then `main` (`T000_main.cpp:105`): `memorycard_warning`, the opening movie
(`stream/opening.pss`), autoload, the menu loop.

## 8. Memory map

### 8.1 The PS2 layout the code assumes

| Range | Content |
|---|---|
| 0x00100000.. | program (.text, .data, .bss) up to `_end` (0x004B2C0C in GT3; larger in the enhanced build) |
| `align16(_end)`..0x01F00000 | the global heap: one `pdistd::Allocator` (`src/engine/pdistd/T532_heap.cpp:35..39`), about 26 MB, zero-filled once by `heap_init_once` |
| 0x01F00000..0x02000000 | main thread stack (1 MB, crt0) |
| 0x70000000..0x70004000 | scratchpad: `GsContext` at 0x70003000 (pgl), MPEG work areas 0x70000000..0x70002368, the movie feeder (0x70001700) and decoder (0x70000B00) thread stacks |
| 0x10000000.., 0x11000000.., 0x12000000.. | EE / VU / GS registers (pgl, media, libkernl) |
| addresses with bit 0x20000000 / 0x30000000 set | uncached / uncached-accelerated views (pgl packet access modes, the MPEG ring buffer) |

### 8.2 Heap and pools

- `malloc` / `memalign` / `realloc` / `calloc` / `free` are pdistd's
  (`T532_heap.cpp`): best-fit `Allocator`, 16-byte minimum alignment, every
  entry point under `intr_suspend`. `memalign(0x40, n)` is used for DMA
  buffers and file buffers (36 call sites) and freed with `free`.
- Sub-allocators with **fixed sizes** taken from the heap: course arena
  0xD00080 (`RaceCourse::initArena`, `T153:51`), car model arena 0x498080
  in 0xA8000 blocks (`RaceCarModel::initArena`, `T180:112`), race pool
  0x22000 (`T007:121`; `RaceCarModel` objects and the race mem pools),
  control records 2 x 0x20000 (`race_begin`), the four menu heaps
  (`T204_menu_heap.cpp`), the two icon heaps (`T077`), the card work buffer
  0xD00000 (`T000:187`), the menu car buffers 2 x 0xA8000 (`T010:265`).
  Nothing sizes a pool from the free heap (the enhanced engine only prints
  it, `T154_race_display.cpp:199`).
- Classes allocated in those pools by their own `operator new`
  (`Object`, `Transitive`, `Operator`, `MemorycardMenuItem` -> current
  menu heap; `RaceCarModel` -> race pool): their MSVC sizes must fit
  pools sized for egcs objects (runtime.md).

### 8.3 Fixed addresses and address arithmetic

- Scratchpad (0x70000000): pgl (`GS_SPR_CTX`, `gs_driver_internal.h:91`
  and the other pgl headers), media (`mpeg_player_internal.h:108..114`,
  `T529_mpegif.cpp:20`), `pgl.c:270` tests `addr & 0x70000000`. Graphics /
  movie rewrites; nothing in game code.
- Uncached views and `& 0x0FFFFFFF` physical masks: DMA tags in pgl
  (`gs_display_driver.c:399`, `render.c:376`, `flare.c:695`, `tex1.cpp:725`,
  `gs_driver.c:432`) and media (`ipu_internal.h:88`,
  `mpeg_player_internal.h:104..105`). Game code: only
  `gfx_set_packet_access(PGL_PKT_UNCACHED_ACCEL)` in `T131_race_ps2_render.cpp:233`.
  A native pointer masked or OR'ed this way is destroyed: these must all go
  with the rewrites, and the game arena should stay below 0x70000000 so a
  forgotten `IS_SPR_ADDR`-style test cannot misfire.
- Pointers cast to `int` / `u32` everywhere (alignment rounding
  `((int)p + 0x3F) & ~0x3F`, the IOP script's `(int)"string"`, relocation
  deltas): fine in a 32-bit process; with `/LARGEADDRESSAWARE` pointers above
  2 GB become negative `int`s (the masks still work, signed compares would
  not). Keep the game arena in the low 2 GB.
- VRAM word addresses (0x50000 boot image, `T002:34`) and SPU2 addresses
  (music bank at 0xAC000, `T463:196`; `pdipipe_read2`'s `buf`) are device
  addresses, not EE memory.
- Alignment: 16 bytes for VU vectors and matrices (`aligned(16)`, 102
  sites), 64 bytes for DMA buffers (`memalign(0x40)`, `aligned(64)`).
  Natively 16 still matters for SSE.

### 8.4 Big static buffers and stack frames

| What | Size | Where |
|---|---|---|
| packet buffers (2) | 2 x 0xA0000 | `T001_packet_buffers.cpp:9..10` |
| render thread stack | 0x8000 | `T002:44` |
| vol thread stack (heap), card manager | 0x8000, 0x1880 | `T503:426`, `T501:333` |
| sector cache | 0x3180 | `T503:34` |
| pad DMA areas, pad thread stack | 8 x 0x100, 0x1000 | `T477:23..24` |
| loader stack, music stack | 0x1000 each | `T508:19`, `T463:46` |
| `play_saved_replay` frame | 0x41980 + `alloca(race size)` | `T000_main.cpp:324` |
| `main_arcade` frame | about 0x360E0 | `T000_main.cpp:534` |
| `LoggerControl::run` + `logger_run` | 0x76070 + 0x42850 | `T055_logger.cpp:1005`, `:1039` |
| `vol_thread_main` frame | 0x3190 + 0x2040 (VLA) | `T503:116..158` |

The main thread's frames reach about 0.75 MB on the console (logger path).

### 8.5 Native memory spec

- One game arena reserved at start-up (`VirtualAlloc`, below 0x70000000),
  pass 1: 128 MB (PORT.md). `heap_start` = arena base, `heap_limit` = arena
  end (`T532_heap.cpp:38..39`); `heap_init_once` zero-fills it on the first
  allocation as before.
- Game threads: native threads with the stack sizes MSVC needs, not the PS2
  ones: the main game thread 16 MB reserved (huge frames, `alloca`, debug
  builds), the others 1 MB. The PS2 stack memory the game passes to
  `CreateThread` is ignored (the scratchpad stacks of the movie threads
  too). The game's `main` runs on a native thread created by the platform,
  not on the Win32 main thread (that one runs the window).
- No scratchpad, no uncached views, no hardware registers in native code.
