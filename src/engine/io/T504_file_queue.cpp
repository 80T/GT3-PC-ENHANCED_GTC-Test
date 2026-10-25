/* T504 - the queued (callback) loads (u111).  0x00241BF0..0x00241E08.
 *
 * file_req_alloc queues a load in a static array of 0x20-byte entries
 * (g_004A3E38[g_00352F98++]), linked in a list sorted by first sector so the
 * disc is read in order; file_req_run (from RaceBase::virtual_8) loads them
 * all, calling each one's callback with the data.
 */
#include "engine/io/file_loader_internal.h"
#include "engine/io/file_missing.h"
#include "engine/io/T504_file_queue.h"

#define FILE_QUEUE_SIZE 32

/* The entries: a file static (.bss, the original's 0x004A3E38..0x004A4238;
 * 0x004A4238..0x004A4240 is the alignment of T508's stack after it). */
static FileQueueEntry g_004A3E38[FILE_QUEUE_SIZE];  /* 0x004A3E38 */

/* PC: the entries past the PS2's 32 (which were written past the array):
 * blocks of FILE_QUEUE_SIZE allocated when first needed and kept (the queue
 * links entries by address, so they never move); the count picks the slot
 * as before. */
static FileQueueEntry **s_moreBlocks;
static int s_moreCount;                         /* blocks allocated */
static int s_moreCap;                           /* room in s_moreBlocks */

/* PC: the slot of count `index' (interrupts off). */
static FileQueueEntry *file_queue_entry(int index)
{
    if (index < FILE_QUEUE_SIZE)
        return &g_004A3E38[index];
    int block = index / FILE_QUEUE_SIZE - 1;
    while (s_moreCount <= block) {
        if (s_moreCount == s_moreCap) {
            int cap = s_moreCap != 0 ? s_moreCap * 2 : 8;
            FileQueueEntry **table = (FileQueueEntry **)malloc(cap * sizeof(FileQueueEntry *));
            if (table == NULL)
                break;
            if (s_moreCount != 0)
                memcpy(table, s_moreBlocks, s_moreCount * sizeof(FileQueueEntry *));
            free(s_moreBlocks);
            s_moreBlocks = table;
            s_moreCap = cap;
        }
        FileQueueEntry *more = (FileQueueEntry *)malloc(FILE_QUEUE_SIZE * sizeof(FileQueueEntry));
        if (more == NULL)
            break;
        if (s_moreCount == 0)
            printf("PC: file queue: more than %d loads queued (the PS2's entries); more kept\n",
                   FILE_QUEUE_SIZE);
        s_moreBlocks[s_moreCount++] = more;
    }
    if (block >= s_moreCount) {
        printf("PC: file queue: no memory for entry %d; it reuses one\n", index);
        return &g_004A3E38[index % FILE_QUEUE_SIZE];
    }
    return &s_moreBlocks[block][index % FILE_QUEUE_SIZE];
}

// FUNC 0x00241BF0 file_req_alloc
//   Takes the next entry (by count: the slot is g_004A3E38[count], so an
//   entry still queued can be reused if the run order differs; the callers
//   queue a batch, then run it), resolves path (retrying on -1) for its
//   sector, and inserts it after every queued entry whose sector is <= its
//   own (signed).  kind 0: file_read_blocking (unpacked), 1: a sound bank
//   (arg = the GTSOUNDINSTRUMENT).
//   PC: no limit of 32 entries (file_queue_entry).
void file_req_alloc(const char *path, void *buf, int size, FileDoneFn done,
                    void *arg, int kind)
{
    FileLoc loc;                                /* sp+0x00 */
    int wasEnabled = intr_suspend();
    FileQueueEntry *entry = file_queue_entry(g_00352F98++);    /* PC: was &g_004A3E38[count++] */

    intr_resume(wasEnabled);

    while (file_resolve(path, &loc, &vol_device_object) == -1)
        file_missing_wait(path);   /* PC: log and yield (a disc never lacked a file) */
    entry->kind = kind;
    entry->path = path;
    entry->buf = buf;
    entry->size = size;
    entry->done = done;
    entry->arg = arg;
    entry->lsn = loc.lsn;

    wasEnabled = intr_suspend();
    FileQueueEntry *prev = NULL;
    FileQueueEntry *next = g_00352F9C;
    while (next != NULL && !(entry->lsn < next->lsn)) {
        prev = next;
        next = next->next;
    }
    entry->next = next;
    if (prev != NULL)
        prev->next = entry;
    else
        g_00352F9C = entry;
    intr_resume(wasEnabled);
}

// FUNC 0x00241D30 file_req_run
//   Pops and loads every queued entry in order; the callback (if any) gets
//   (arg, data); an unknown kind gives data NULL.  A sound bank goes to the
//   instrument's GTSOUNDINSTRUMENT::nextSpuAddr.
void file_req_run(void)
{
    for (;;) {
        int wasEnabled = intr_suspend();
        FileQueueEntry *entry = g_00352F9C;
        if (entry != NULL) {
            g_00352F9C = entry->next;
            g_00352F98--;
        }
        intr_resume(wasEnabled);
        if (entry == NULL)
            break;

        void *data = NULL;
        switch (entry->kind) {
        case FILE_QUEUE_FILE:
            data = file_read_blocking(entry->path, entry->buf, entry->size, 1,
                                      &vol_device_object);
            break;
        case FILE_QUEUE_SOUND_BANK: {
            GTSOUNDINSTRUMENT *inst = (GTSOUNDINSTRUMENT *)entry->arg;
            data = file_read_sound_bank(entry->path, inst->nextSpuAddr(),
                                        entry->buf, entry->size);
            break;
        }
        }
        if (entry->done != NULL)
            entry->done(entry->arg, data);
    }
}

/* The count and the queue: uninitialised globals, which egcs writes as
 * zeros at the end of the file (.sdata), in file_loader_internal.h's declaration order. */
volatile int g_00352F98;                        /* 0x00352F98 gp-0x4A58 */
FileQueueEntry *volatile g_00352F9C;            /* 0x00352F9C gp-0x4A54 */
