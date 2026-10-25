/* file_missing.cpp - see file_missing.h. */
#include "engine/io/file_missing.h"
#include "eekernel.h"

/* The last few paths reported (a game thread retries one path at a time,
 * so a short list is enough to log each missing file once per stretch). */
static char s_reported[8][IO_PATH_MAX];
static int  s_next;

void file_missing_wait(const char *path)
{
    if (path == NULL)
        path = "(null)";
    int was = DIntr();
    int known = 0;
    for (int i = 0; i < 8; i++)
        if (strncmp(s_reported[i], path, sizeof(s_reported[0]) - 1) == 0)
            known = 1;
    if (!known) {
        strncpy(s_reported[s_next], path, sizeof(s_reported[0]) - 1);
        s_next = (s_next + 1) & 7;
    }
    if (was)
        EI();
    if (!known)
        printf("file: %s is not in the game data; waiting for it\n", path);
    DelayThread(100000);
}

/* io_once's table: FNV-1a hashes (0 = free), open addressing. */
#define IO_ONCE_SLOTS 4096
static u32 s_once[IO_ONCE_SLOTS];

static u32 io_hash(u32 h, const char *s)
{
    if (s != NULL)
        for (; *s != '\0'; s++)
            h = (h ^ (u8)*s) * 16777619u;
    return (h ^ 0xFF) * 16777619u;              /* the separator */
}

int io_once(const char *what, const char *path)
{
    u32 h = io_hash(io_hash(2166136261u, what), path);
    if (h == 0)
        h = 1;
    int first = 0;
    int was = DIntr();
    for (int i = 0; i < IO_ONCE_SLOTS; i++) {
        u32 *slot = &s_once[(h + i) & (IO_ONCE_SLOTS - 1)];
        if (*slot == h)
            break;                              /* seen */
        if (*slot == 0) {
            *slot = h;
            first = 1;
            break;
        }
    }                                           /* full: no more lines */
    if (was)
        EI();
    return first;
}

int io_path_join(char *dst, int cap, const char *a, const char *b, const char *c)
{
    const char *parts[3] = { a, b, c };
    int n = 0;
    int whole = 1;

    if (cap <= 0)
        return 0;
    for (int i = 0; i < 3; i++) {
        const char *s = parts[i];
        if (s == NULL)
            continue;
        for (; *s != '\0'; s++) {
            if (n >= cap - 1) {
                whole = 0;
                break;
            }
            dst[n++] = *s;
        }
    }
    dst[n] = '\0';
    if (!whole && io_once("path", dst))
        printf("PC: path too long for its %d-byte buffer, cut: %s%s%s\n", cap, a ? a : "", b ? b : "",
               c ? c : "");
    return whole;
}
