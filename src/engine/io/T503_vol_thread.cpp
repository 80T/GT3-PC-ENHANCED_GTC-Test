/* T503 - the volume reader thread (u111).  0x00240FA0..0x00241BF0.
 *
 * One request at a time, in vol_thread_object (VolThread):
 *   file_request   resolves the file (on vol_device_object, whatever device
 *                  the caller passed), allocates the buffer if none, and sets
 *                  the request unless the thread is busy (0 then);
 *   file_req_wait  marks it busy and signals the thread;
 *   file_req_result
 *                  waits (SleepThread) on a FileWaiter for the end, returns
 *                  the buffer.
 * The thread opens the file through the IOP pipe (pdipipe_open), reads it in
 * 0x40000-byte pieces, or inflates it (GZIP::InflateLoader, at priority 9),
 * then hands the result to every FileWaiter on the list and wakes them.
 * file_req_cancel stops a read between pieces (a buffer file_request
 * allocated is freed then).
 *
 * The functions take `VolThread *vt = &vol_thread_object' first: retail
 * computes the address at the entry, before the first call, and keeps it
 * in a saved register.
 */
#include "engine/io/file_loader_internal.h"
#include "engine/io/file_missing.h"             /* PC: also io_path_join, io_once, IO_PATH_MAX */
#include "engine/io/T503_vol_thread.h"
#include "engine/sound/spu_memory.h"            /* PC: spu_memory_name (the sound trace) */

#define VOL_THREAD_PRIORITY     4
#define VOL_INFLATE_PRIORITY    9
#define VOL_THREAD_STACK_SIZE   0x8000
#define INFLATE_WINDOW_SIZE     0x2000
#define SOUND_BANK_HEAD_SIZE    0x20            /* the part of SoundBankHeader read first */
#define SOUND_BANK_MAGIC_ENGN   0x4E474E45      /* "ENGN" */
#define SOUND_BANK_MAGIC_INST   0x54534E49      /* "INST" */

/* The disc sector cache (vol_init gives it to sector_cache_init): a file
 * static (.bss, the original's 0x004A0CB8..0x004A3E38). */
static u8 vol_cache_buffer[0x3180];             /* 0x004A0CB8 */

// FUNC 0x00240FA0 vol_waiter_listed
//   ? name.  Whether w is on the waiter list (interrupts off).
int vol_waiter_listed(FileWaiter *w)
{
    VolThread *vt = &vol_thread_object;
    int wasEnabled = intr_suspend();
    FileWaiter *p = vt->waiters;

    while (p != NULL && p != w)
        p = p->next;
    intr_resume(wasEnabled);
    return p == w;
}

// FUNC 0x00241018 vol_set_waiters_status
//   ? name.  Sets the status of every waiter (interrupts off).
void vol_set_waiters_status(int status)
{
    int wasEnabled = intr_suspend();
    FileWaiter *p;

    for (p = vol_thread_object.waiters; p != NULL; p = p->next)
        p->status = status;
    intr_resume(wasEnabled);
}

// FUNC 0x00241078 vol_open_name
//   ? name.  What to open for a resolved file of dev: *path becomes the
//   host: file name built in nameBuf (types 2, 3) or NULL (the disc: types
//   0, 1); *lsn becomes the absolute sector (type 0: + the volume's), the
//   byte offset in the host: volume file (type 2: << 11) or 0 (type 3,
//   ".gz" appended when unpacking).  nameBuf is cleared when *path ends up
//   NULL.  nameBufSize is not used (the register is reused at once).
//   PC: it is: the name is cut to nameBufSize bytes (logged once).
void vol_open_name(VolDevice *dev, const char **path, int *lsn, int unpack,
                   char *nameBuf, int nameBufSize)
{
    switch (dev->type) {
    case VOL_DEV_DISC_VOLUME: {
        *path = NULL;
        VolExtent extent = dev->extent;         /* sp+0x00, copied by value */
        *lsn += extent.lsn;
        break;
    }
    case VOL_DEV_DISC_FILES:
        *path = NULL;
        break;
    case VOL_DEV_HOST_VOLUME:
        io_path_join(nameBuf, nameBufSize, "host:", dev->name, NULL);   /* PC: bounded */
        *path = nameBuf;
        *lsn <<= VOL_SECTOR_SHIFT;
        break;
    case VOL_DEV_HOST_FILES:
        io_path_join(nameBuf, nameBufSize, "host:data/", *path, unpack ? ".gz" : NULL);   /* PC: bounded */
        *lsn = 0;
        *path = nameBuf;
        break;
    }
    if (*path == NULL)
        nameBuf[0] = '\0';
}

// FUNC 0x002411E8 vol_thread_main
//   The reader thread (priority 4, 0x8000-byte stack, started by vol_init).
//   Each wake-up of the semaphore: copy the request, mark the waiters
//   "reading"; if a request is pending, open the file and read req.size
//   bytes in pieces of at most 0x40000 (stopping on cancel), or inflate it
//   (the 0x2000-byte window is a 64-aligned block of a 0x2040-byte
//   variable-size array, freed after the inflate); then
//   status done or cancelled (a buffer file_request allocated is freed, the
//   result is NULL), close, and hand {status, buf, size} to each waiter,
//   waking its thread.  The size handed is the unpacked size when unpacking,
//   else the packed (stored) one.  Exits (worker_thread_exit) when quit is
//   set.
//   PC: the inflate stops at the buffer's end (req.size bytes: the caller's
//   size, or the file's unpacked size rounded up to 64), logged once per
//   file, as a damaged .gz is.
void vol_thread_main(void *arg)
{
    VolThread *vt = &vol_thread_object;
    FileRequest req;                            /* fp+0x0000 */
    char hostName[IO_PATH_MAX];                 /* fp+0x0030 (PC: was 0x100 bytes) */
    const char *openPath;                       /* fp+0x3180 */
    int openLsn;                                /* fp+0x3184 */

    for (;;) {
        WaitSema(vt->sema);
        if (vt->quit != 0)
            break;

        int wasEnabled = intr_suspend();
        int pending = vt->pending;
        req = vt->req;
        vol_set_waiters_status(FILE_WAIT_READING);
        intr_resume(wasEnabled);
        if (pending == 0)
            continue;

        openPath = req.path;
        openLsn = req.loc.lsn;
        int fileSize = req.loc.packedSize;
        if (req.unpack)
            fileSize = req.loc.size;
        vol_open_name(req.dev, &openPath, &openLsn, req.unpack, hostName, sizeof(hostName));
        int stream = pdipipe_open(openLsn, fileSize, VOL_READ_CHUNK, openPath);
        vt->handle = stream;

        u8 *data = (u8 *)req.buf;
        if (data == NULL) {
            /* PC: no buffer (the game heap was full): nothing is read, the
             * result is NULL */
        } else if (req.unpack == 0) {
            int left = req.size;
            u8 *dst = data;
            while (left != 0 && vt->cancel == 0) {
                int chunk = min_int(left, VOL_READ_CHUNK);
                pdipipe_read(stream, dst, chunk);
                left -= chunk;
                dst += chunk;
            }
        } else {
            /* the window: a 64-aligned block of a 0x2040-byte variable-size
             * array (sp -= 0x2040, restored after the block; $fp is the
             * frame pointer) */
            int windowSize = INFLATE_WINDOW_SIZE;
            u8 windowRaw[INFLATE_WINDOW_SIZE + 0x40];  /* PC: a fixed array (was a GNU VLA) */
            ChangeThreadPriority(GetThreadId(), VOL_INFLATE_PRIORITY);
            /* the mask in a variable, as in file_read_sound_bank: retail ties
             * the sum, not the mask, to the argument register */
            int alignMask = ~0x3F;
            void *window = (void *)(((int)windowRaw + 0x3F) & alignMask);
            /* a temporary at fp+0x130: its constructor inflates the whole
             * stream into data, then ~InflateLoader (flags 2) */
            {
                /* PC: named, with the output end (was a temporary without one) */
                GZIP::InflateLoader loader(stream, data, (u32)req.size, window, windowSize);
                if (loader.stopped != GZIP::GZ_STOP_NONE && io_once("inflate", req.path)) {
                    if (loader.stopped == GZIP::GZ_STOP_OUTPUT_END)
                        printf("PC: %s.gz unpacks to more than %d bytes: cut there\n", req.path, req.size);
                    else
                        printf("PC: %s.gz is damaged (%s): unpacked in part\n", req.path,
                               loader.stopped == GZIP::GZ_STOP_TABLES ? "Huffman tables over the arena"
                                                                      : "a distance before the start");
                }
            }
            ChangeThreadPriority(GetThreadId(), VOL_THREAD_PRIORITY);
        }

        int status = FILE_WAIT_DONE;
        wasEnabled = intr_suspend();
        vt->busy = 0;
        vt->pending = 0;
        if (vt->cancel != 0) {
            if (req.allocated) {
                free(data);
                data = NULL;
            }
            status = FILE_WAIT_CANCELLED;
        }
        FileWaiter *w = vt->waiters;
        vt->resultBuf = data;
        vt->resultStatus = status;
        vt->waiters = NULL;
        intr_resume(wasEnabled);

        pdipipe_close(stream);

        while (w != NULL) {
            wasEnabled = intr_suspend();
            int thread = w->thread;
            w->status = status;
            w->buf = data;
            w->size = fileSize;
            w->thread = -1;
            w = w->next;
            intr_resume(wasEnabled);
            if (thread != -1)
                WakeupThread(thread);
        }
    }
    worker_thread_exit(&vt->worker);
}

// FUNC 0x002414A8 vol_add_waiter
//   ? name.  Puts w at the head of the waiter list (status queued, buf NULL)
//   unless it is already on it, and sets the thread to wake (-1 none).
//   NULL w: nothing.
void vol_add_waiter(FileWaiter *w, int thread)
{
    VolThread *vt = &vol_thread_object;

    if (w == NULL)
        return;

    int wasEnabled = intr_suspend();
    if (!vol_waiter_listed(w)) {
        w->status = FILE_WAIT_QUEUED;
        w->next = vt->waiters;
        vt->waiters = w;
        w->buf = NULL;
    }
    w->thread = thread;
    intr_resume(wasEnabled);
}

// FUNC 0x00241530 file_request
//   Resolves path (retrying while file_resolve gives -1; always on
//   vol_device_object: dev is only stored in the request), and if the
//   thread is not busy sets the request and adds w (no thread) to the
//   waiters: 1.  Busy: 0.  size 0 = the file's size (unpacked if unpack,
//   else stored) rounded up to 64; buf NULL = memalign(64, size) (which
//   leaks when the thread is busy: file_read_blocking then retries and
//   allocates again).  unpack only stays set for a packed file.
//   PC: a file bigger than an explicit size is logged once ("PC: <path> is
//   N bytes, read into M": it is cut there, as before); the buffer allocated
//   here is freed again when the thread is busy (no leak per retry).  A
//   packed file's size is its .gz's ISIZE (file_resolve).
int file_request(VolDevice *dev, const char *path, void *buf, int size,
                 int unpack, FileWaiter *w)
{
    VolThread *vt = &vol_thread_object;
    FileLoc loc;                                /* sp+0x00 */

    while (file_resolve(path, &loc, &vol_device_object) == -1)
        file_missing_wait(path);   /* PC: log and yield (a disc never lacked a file) */

    int fileSize = unpack ? (int)loc.size : (int)loc.packedSize;
    int allocated = 0;
    if (size != 0 && fileSize > size && io_once("cut", path))   /* PC */
        printf("PC: %s is %d bytes, read into %d\n", path, fileSize, size);
    fileSize = (fileSize + 0x3F) & ~0x3F;
    if (size == 0)
        size = fileSize;
    if (!(loc.flags & FILE_LOC_PACKED))
        unpack = 0;
    if (buf == NULL) {
        allocated = 1;
        buf = memalign(0x40, size);
        if (buf == NULL && io_once("nomem", path))  /* PC: the game heap is full */
            printf("PC: no memory for %s (%d bytes)\n", path, size);
    }

    int wasEnabled = intr_suspend();
    int taken = 0;
    if (vt->busy == 0) {
        vt->req.dev = dev;
        vt->pending = 1;
        vt->req.path = path;
        vt->cancel = 0;
        vt->req.loc = loc;
        vt->req.allocated = allocated;
        vt->req.buf = buf;
        vt->req.size = size;
        vt->req.unpack = unpack;
        vol_add_waiter(w, -1);
        taken = 1;
    }
    intr_resume(wasEnabled);
    if (!taken && allocated)
        free(buf);                  /* PC: the retry allocates again (retail leaked it) */
    return taken;
}

// FUNC 0x00241690 file_req_wait
//   Hands the request to the thread: busy = 1, signal its semaphore.
void file_req_wait(void)
{
    vol_thread_object.busy = 1;
    signal_sema_helper(vol_thread_object.sema);
}

// FUNC 0x002416C0 file_req_result
//   Not busy: w gets the last result at once.  Busy: w waits with this
//   thread (GetThreadId) and sleeps until the reader thread clears
//   w->thread.  Returns w->buf.
void *file_req_result(FileWaiter *w)
{
    VolThread *vt = &vol_thread_object;
    int wasEnabled = intr_suspend();
    int mustWait;

    if (vt->busy == 0) {
        mustWait = 0;
        w->buf = vt->resultBuf;
        w->status = vt->resultStatus;
        w->thread = -1;
    } else {
        mustWait = 1;
        vol_add_waiter(w, GetThreadId());
    }
    intr_resume(wasEnabled);

    while (mustWait) {
        SleepThread();
        if (w->thread == -1)
            break;
    }
    return w->buf;
}

// FUNC 0x00241778 file_req_cancel
//   ? name.  Sets cancel while the thread is busy.  Callers: T508.
void file_req_cancel(void)
{
    VolThread *vt = &vol_thread_object;
    int wasEnabled = intr_suspend();

    if (vt->busy != 0)
        vt->cancel = 1;
    intr_resume(wasEnabled);
}

// FUNC 0x002417C0 file_read_blocking
//   Reads path into buf (NULL: allocated, 64-aligned), waiting first for a
//   running request (file_req_wait_idle), then for this one.  Returns the
//   buffer (NULL if cancelled and allocated here).  unpack: inflate a
//   packed (.gz) file.
void *file_read_blocking(const char *path, void *buf, int size, int unpack,
                         VolDevice *dev)
{
    FileWaiter w;                               /* sp+0x00 */

    while (!file_request(dev, path, buf, size, unpack, &w))
        file_req_wait_idle();
    file_req_wait();
    return file_req_result(&w);
}

/* PC (no original): the bytes of path as file_request counts them: the
 * unpacked size (a .gz's ISIZE) when unpack and the file is packed, else
 * the stored size; not rounded.  Waits for a missing file. */
static int vol_file_bytes(const char *path, int unpack)
{
    FileLoc loc;

    while (file_resolve(path, &loc, &vol_device_object) == -1)
        file_missing_wait(path);
    return (unpack && (loc.flags & FILE_LOC_PACKED)) ? (int)loc.size : (int)loc.packedSize;
}

/* PC (no original): see T503_vol_thread.h. */
int file_read_size(const char *path, int unpack, VolDevice *dev)
{
    (void)dev;                                  /* as file_request: vol_device_object */
    return (vol_file_bytes(path, unpack) + 0x3F) & ~0x3F;
}

/* PC (no original): see T503_vol_thread.h. */
void *file_read_blocking_cap(const char *path, void *buf, int capacity, int unpack,
                             VolDevice *dev)
{
    if (buf == NULL)
        return file_read_blocking(path, NULL, 0, unpack, dev);
    int bytes = (vol_file_bytes(path, unpack) + 0x3F) & ~0x3F;
    if (bytes > capacity)
        bytes = capacity;                       /* file_request logs the cut */
    if (bytes <= 0)
        return buf;                             /* nothing (size 0 would read the whole file) */
    return file_read_blocking(path, buf, bytes, unpack, dev);
}

/* PC (no original): see T503_vol_thread.h. */
void *file_read_blocking_fit(const char *path, void *buf, int capacity, int unpack,
                             VolDevice *dev, int *allocated)
{
    void *data;

    if (allocated != NULL)
        *allocated = 0;
    int bytes = buf != NULL ? vol_file_bytes(path, unpack) : 0;
    if (buf != NULL && bytes <= capacity)
        return file_read_blocking_cap(path, buf, capacity, unpack, dev);
    if (buf != NULL && io_once("fit", path))
        printf("PC: %s is %d bytes, more than its %d-byte buffer: read into a block of its own\n", path,
               bytes, capacity);
    data = file_read_blocking(path, NULL, 0, unpack, dev);
    if (allocated != NULL && data != NULL)
        *allocated = 1;
    return data;
}

// FUNC 0x00241858 file_read_async
//   ? name.  Starts a read with no waiter: 1 if the thread took it, else 0.
//   No callers.
int file_read_async(const char *path, void *buf, int size, int unpack,
                    VolDevice *dev)
{
    if (file_request(dev, path, buf, size, unpack, NULL)) {
        file_req_wait();
        return 1;
    }
    return 0;
}

// FUNC 0x002418B0 file_req_wait_idle
//   ? name.  Waits for the running request to end (its result discarded).
void file_req_wait_idle(void)
{
    FileWaiter w;                               /* sp+0x00 */

    file_req_result(&w);
}

// FUNC 0x002418D0 file_read_sound_bank
//   Loads a sound bank synchronously, on the caller's thread, bypassing the
//   reader thread: reads the 0x20-byte head of the SoundBankHeader into a
//   64-aligned stack block (a 0x80 alloca); if it is "ENGN" or "INST", reads
//   the rest of the header part (+0x14 bytes from the start, less the 0x20)
//   into buf (NULL: memalign(64, size, or that header size if size is 0)),
//   moves it up 0x20 and puts the head in front, then streams spuSize bytes
//   of samples to SPU address spuAddr (pdipipe_read2, toSpu).  Returns buf
//   (unchanged / NULL for an unknown head).  Callers:
//   GTSOUNDINSTRUMENT::loadFile (0x0022C140), file_req_run (sound banks).
//   PC: a header part bigger than size (the caller's buffer) is read into a
//   block of its own size instead (memalign(64), the game heap: free() it;
//   the caller's buffer is left as it is), logged once; a header part
//   shorter than the head is damaged: nothing more is read, buf returned.
void *file_read_sound_bank(const char *path, int spuAddr, void *buf, int size)
{
    FileLoc loc;                                /* fp+0x000 */
    char hostName[IO_PATH_MAX];                 /* fp+0x010 (PC: was 0x100 bytes) */
    int openLsn;                                /* fp+0x114 */
    /* sp -= 0x80 ($fp is the frame pointer).  The mask is set before the
     * alloca: retail loads it first ($v1; written in place, it comes after
     * the sum and takes $v0). */
    int alignMask = ~0x3F;
    SoundBankHeader *head =
        (SoundBankHeader *)(((int)__builtin_alloca(0x80) + 0x3F) & alignMask);

    while (file_resolve(path, &loc, &vol_device_object) == -1)
        file_missing_wait(path);   /* PC: log and yield (a disc never lacked a file) */
    const char *bankName = path;   /* PC: for the sound trace (vol_open_name clears path) */
    openLsn = loc.lsn;
    int storedSize = loc.packedSize;
    vol_open_name(&vol_device_object, &path, &openLsn, 0, hostName, sizeof(hostName));
    int stream = pdipipe_open(openLsn, storedSize, VOL_READ_CHUNK, path);

    pdipipe_read2(stream, head, SOUND_BANK_HEAD_SIZE, 0);
    if (head->magic == SOUND_BANK_MAGIC_ENGN || head->magic == SOUND_BANK_MAGIC_INST) {
        /* in the file, the offset of the samples = the bytes of the header
         * part */
        int headerSize = (int)head->samples;
        if (headerSize < SOUND_BANK_HEAD_SIZE) {
            /* PC: shorter than the head itself: damaged (the moves below
             * would run backwards over memory) */
            if (io_once("bank", bankName))
                printf("PC: %s: a %d-byte sound bank header is damaged; not loaded\n", bankName, headerSize);
            pdipipe_close(stream);
            return buf;
        }
        if (size == 0)
            size = headerSize;
        if (size < headerSize) {
            /* PC: the header does not fit the caller's buffer: a block of
             * its own (the caller's is left as it is) */
            if (io_once("bankbuf", bankName))
                printf("PC: %s: its %d-byte sound bank header is bigger than the %d-byte buffer: "
                       "read into a block of its own\n", bankName, headerSize, size);
            size = headerSize;
            buf = NULL;
        }
        if (buf == NULL)
            buf = memalign(0x40, size);
        if (buf == NULL) {
            printf("PC: no memory for the %d-byte header of %s\n", size, bankName);   /* PC */
            pdipipe_close(stream);
            return NULL;
        }
        pdipipe_read2(stream, buf, headerSize - SOUND_BANK_HEAD_SIZE, 0);
        memmove((u8 *)buf + SOUND_BANK_HEAD_SIZE, buf, headerSize - SOUND_BANK_HEAD_SIZE);
        memcpy(buf, head, SOUND_BANK_HEAD_SIZE);
        /* PC: the sound trace names the memory the samples fill
         * (GT3PC_SND_TRACE, docs/sound.md 3.6; nothing when off) */
        spu_memory_name((u32)spuAddr, (u32)((SoundBankHeader *)buf)->spuSize, bankName);
        pdipipe_read2(stream, (void *)spuAddr, ((SoundBankHeader *)buf)->spuSize, 1);
    }
    pdipipe_close(stream);
    return buf;
}

// FUNC 0x00241A68 vol_init
//   Once: the disc sector cache (vol_cache_buffer, 0x3180 bytes), mounts
//   volName on the disc (type 0, TOC size from its header), starts the
//   memory-card manager (mc_manager_init), then the reader thread (priority
//   4, a 64-aligned 0x8000-byte stack).  Caller: game init (0x00102C00).
//   PC: the volume is mounted as the loose host:data/ files (type 3), the
//   form the development builds read from the PC: the launcher unpacks
//   GT3.VOL into the game folder's data\ (platform/paths.h), and nothing is
//   read from a disc or a volume file.
void vol_init(const char *volName)
{
    if (vol_init_guard != 0)
        return;

    vol_init_guard = 1;
    sector_cache_init(vol_cache_buffer, sizeof(vol_cache_buffer));
    vol_mount(&vol_device_object, VOL_DEV_HOST_FILES, volName, 0);   /* PC: was VOL_DEV_DISC_VOLUME */
    mc_manager_init();
    memset(&vol_thread_object, 0, sizeof(vol_thread_object));
    vol_thread_object.quit = 0;
    vol_thread_object.sema = create_mutex_sema(0);
    void *stack = memalign(0x40, VOL_THREAD_STACK_SIZE);
    vol_thread_object.stack = stack;
    worker_thread_start(&vol_thread_object.worker, vol_thread_main, NULL,
                        VOL_THREAD_PRIORITY, stack, VOL_THREAD_STACK_SIZE);
}

// FUNC 0x00241B20 vol_shutdown
//   Undoes vol_init: quit = 1, wakes the thread and waits for its end
//   (worker_thread_join), then worker_thread_exit on the same ThreadCtx (as
//   the original does: that is the thread-exit helper, here called by the
//   caller), deletes the semaphore, frees the stack and the TOC
//   (VolDevice::freeToc), stops the memory-card manager.  No callers.
void vol_shutdown(void)
{
    VolThread *vt = &vol_thread_object;

    if (vol_init_guard == 0)
        return;

    vt->quit = 1;
    vol_init_guard = 0;
    signal_sema_helper(vt->sema);
    worker_thread_join(&vt->worker);
    worker_thread_exit(&vt->worker);
    DeleteSema(vt->sema);
    free(vt->stack);
    vol_device_object.freeToc();
    mc_manager_shutdown();
}

// FUNC 0x00241B98 F_00241B98
//   min(a, b) (signed): T503's own out-of-line copy of the helper that T501
//   has at 0x00240AE0 (min_int, the one vol_thread_main calls).  No callers.
int F_00241B98(int a, int b)
{
    return (a < b) ? a : b;
}

/* The gt3.vol device (.data 0x002F33B0: egcs puts a public object that has
 * a constructor in .data, not in .bss; vol_thread_object follows it). */
VolDevice vol_device_object;

/* The reader thread's record and vol_init's guard: uninitialised globals,
 * which egcs writes as zeros at the end of the file (.data after
 * vol_device_object, .sdata after ".gz"), in file_loader_internal.h's declaration order. */
VolThread vol_thread_object;                    /* 0x002F33E0 */
int vol_init_guard;                             /* 0x00352F94 gp-0x4A5C */

/* Compiler-generated: egcs's static destructor and initialiser of
 * vol_device_object (.dtors entry 0x0028F278, .ctors entry 0x0028F1A4),
 * emitted at the end of the unit, the destructor first: ~VolDevice (flags
 * 2) / VolDevice() on it. */
// FUNC 0x00241BA8 T503_static_dtor (_GLOBAL_$D$vol_waiter_listed__FP10FileWaiter)
/* implicit: ~VolDevice (the static destructor of vol_device_object) */

// FUNC 0x00241BD0 T503_static_init (_GLOBAL_$I$vol_waiter_listed__FP10FileWaiter)
/* implicit: VolDevice (the static initialiser of vol_device_object) */
