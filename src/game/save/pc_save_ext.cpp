/* pc_save_ext.cpp - PC (ROADMAP 18, no original): the save records past their
 * retail capacities and the name-keyed save lists (pc_save_ext.h). */
#include "game/save/pc_save_ext.h"
#include "libc.h"                               /* memcpy, memset, printf */

#include <mutex>
#include <vector>

namespace {

struct ExtEntry {
    const void      *owner;
    int              tag;
    std::vector<u8>  data;                      /* the elements past the fixed array */
    std::vector<u8>  kept;                      /* kept keyed-list entries, raw */
    u32              keptCount;
};

/* Few owners (the GameStatus's records, the play lists): a list.  Never
 * destroyed (a play list's destructor may run after this file's statics
 * at exit), created at the first use (game_init, the main thread). */
std::vector<ExtEntry> &ext_list()
{
    static std::vector<ExtEntry> *list = new std::vector<ExtEntry>;
    return *list;
}
std::mutex &ext_lock()
{
    static std::mutex *lock = new std::mutex;
    return *lock;
}
#define g_ext     ext_list()
#define g_extLock ext_lock()

ExtEntry *find_entry(const void *owner, int tag, bool create)
{
    for (size_t i = 0; i < g_ext.size(); i++)
        if (g_ext[i].owner == owner && g_ext[i].tag == tag)
            return &g_ext[i];
    if (!create)
        return NULL;
    ExtEntry e;
    e.owner = owner;
    e.tag = tag;
    e.keptCount = 0;
    g_ext.push_back(e);
    return &g_ext.back();
}

} // namespace

u8 *pc_ext_get(const void *owner, int tag, u32 bytes, u32 *oldBytes)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, bytes != 0);

    if (e == NULL) {
        if (oldBytes != NULL)
            *oldBytes = 0;
        return NULL;
    }
    if (oldBytes != NULL)
        *oldBytes = (u32)e->data.size();
    if (e->data.size() < bytes)
        e->data.resize(bytes, 0);
    return e->data.empty() ? NULL : &e->data[0];
}

u32 pc_ext_size(const void *owner, int tag)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, false);

    return e != NULL ? (u32)e->data.size() : 0;
}

void pc_ext_resize(const void *owner, int tag, u32 bytes)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, bytes != 0);

    if (e != NULL)
        e->data.resize(bytes, 0);
}

void pc_ext_copy(const void *dst, const void *src, int tag)
{
    if (dst == src)
        return;
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *s = find_entry(src, tag, false);
    ExtEntry *d = find_entry(dst, tag, s != NULL);

    if (d == NULL)
        return;
    s = find_entry(src, tag, false);            /* (the list may have moved) */
    if (s == NULL) {
        d->data.clear();
        d->kept.clear();
        d->keptCount = 0;
        return;
    }
    d->data = s->data;
    d->kept = s->kept;
    d->keptCount = s->keptCount;
}

void pc_ext_release(const void *owner, int tag)
{
    std::lock_guard<std::mutex> lock(g_extLock);

    for (size_t i = 0; i < g_ext.size(); i++)
        if (g_ext[i].owner == owner && g_ext[i].tag == tag) {
            g_ext.erase(g_ext.begin() + i);
            return;
        }
}

void pc_kept_clear(const void *owner, int tag)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, false);

    if (e != NULL) {
        e->kept.clear();
        e->keptCount = 0;
    }
}

void pc_kept_add(const void *owner, int tag, const u8 *entry, u32 bytes)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, true);

    e->kept.insert(e->kept.end(), entry, entry + bytes);
    e->keptCount++;
}

const u8 *pc_kept_get(const void *owner, int tag, u32 *count, u32 *bytes)
{
    std::lock_guard<std::mutex> lock(g_extLock);
    ExtEntry *e = find_entry(owner, tag, false);

    if (e == NULL || e->keptCount == 0) {
        *count = 0;
        *bytes = 0;
        return NULL;
    }
    *count = e->keptCount;
    *bytes = (u32)e->kept.size();
    return &e->kept[0];
}

/* ------------------------------------------------------------------------ */
/* The stream                                                               */
/* ------------------------------------------------------------------------ */

void pc_put(PcSaveWriter *w, const void *src, u32 n)
{
    if (w->p != NULL) {
        memcpy(w->p, src, n);
        w->p += n;
    }
    w->size += n;
}

void pc_put_u32(PcSaveWriter *w, u32 v)
{
    pc_put(w, &v, 4);
}

void pc_keyed_begin(PcKeyedList *l, PcSaveWriter *w)
{
    l->w = w;
    l->countAt = w->p;
    l->count = 0;
    pc_put_u32(w, 0);                           /* the count, stored by pc_keyed_end */
}

void pc_keyed_put(PcKeyedList *l, const void *key, u32 keyLen, const void *rec, u32 recSize)
{
    u16 len = (u16)keyLen;

    pc_put(l->w, &len, 2);
    pc_put(l->w, key, len);
    pc_put(l->w, rec, recSize);
    l->count++;
}

void pc_keyed_end(PcKeyedList *l, const void *owner, int tag)
{
    u32 keptCount, keptBytes;
    const u8 *kept;
    std::vector<u8> copy;

    {
        /* a copy: pc_put runs outside the lock */
        std::lock_guard<std::mutex> lock(g_extLock);
        ExtEntry *e = find_entry(owner, tag, false);
        keptCount = e != NULL ? e->keptCount : 0;
        if (keptCount != 0)
            copy = e->kept;
    }
    keptBytes = (u32)copy.size();
    kept = keptBytes != 0 ? &copy[0] : NULL;
    if (kept != NULL)
        pc_put(l->w, kept, keptBytes);
    l->count += keptCount;
    if (l->countAt != NULL)
        memcpy(l->countAt, &l->count, 4);
}

const u8 *pc_keyed_skip(const u8 *src, const u8 *end, u32 recSize)
{
    u32 count, i;

    if (src == NULL || src > end || end - src < 4)
        return NULL;
    memcpy(&count, src, 4);
    src += 4;
    for (i = 0; i < count; i++) {
        u16 len;

        if (end - src < 2)
            return NULL;
        memcpy(&len, src, 2);
        src += 2;
        if ((u32)(end - src) < (u32)len + recSize)
            return NULL;
        src += len + recSize;
    }
    return src;
}

const u8 *pc_keyed_load(const u8 *src, const u8 *end, u32 recSize, const void *owner, int tag,
                        PcKeyedPlace place, void *ctx, const char *what)
{
    u32 count, i, kept = 0;
    char key[0x100];

    pc_kept_clear(owner, tag);
    if (src == NULL || end - src < 4)
        return NULL;
    memcpy(&count, src, 4);
    src += 4;
    for (i = 0; i < count; i++) {
        const u8 *entry = src;
        u16 len;

        if (end - src < 2)
            return NULL;
        memcpy(&len, src, 2);
        src += 2;
        if ((u32)(end - src) < (u32)len + recSize)
            return NULL;
        int placed = 0;
        if (len < sizeof(key)) {
            memcpy(key, src, len);
            key[len] = '\0';
            placed = place(ctx, key, len, src + len);
        }
        src += len + recSize;
        if (!placed) {
            pc_kept_add(owner, tag, entry, (u32)(src - entry));
            kept++;
        }
    }
    if (kept != 0)
        printf("PC: GameStatus load: %u %s not in the current data: kept, written back by the next save\n",
               kept, what);
    return src;
}
