/* pc_menu_tires.cpp - the race cars' menu tyres (pc_menu_tires.h).
 * No original: new PC code. */
#include "game/app/pc_menu_tires.h"
#include "game/car/model/T447_tire_model_base.h"    /* TireFile2, tire_file_path / _relocate */
#include "engine/io/file_loader_internal.h"
#include "engine/io/T502_file_resolve.h"
#include "engine/io/T503_vol_thread.h"
#include "engine/io/T510_vol_device.h"
#include "platform/settings.h"
#include "libc.h"

int pc_menu_tires(void)
{
    return settings::menu_tires() ? 1 : 0;
}

namespace {

/* The menu tyre files read, one per tyre number, counted by their users. */
enum { kFiles = 64 };                       /* (the game has 39 tyres) */
struct Shared {
    int number;
    TireFile2 *file;                        /* NULL: free */
    u32 size;
    int users;
};
Shared g_files[kFiles];

}  /* namespace */

TireFile2 *pc_menu_tire_acquire(int number, u32 *size)
{
    Shared *spare = NULL;
    for (Shared &s : g_files) {
        if (s.file != NULL && s.number == number) {
            s.users++;
            if (size != NULL)
                *size = s.size;
            return s.file;
        }
        if (s.file == NULL && spare == NULL)
            spare = &s;
    }
    char path[0x40];
    FileLoc loc;
    tire_file_path(path, number, 1);
    if (spare == NULL) {
        printf("PC: menu tyre %s: no place for another tyre file, the race tyre drawn\n", path);
        return NULL;
    }
    if (file_resolve(path, &loc, &vol_device_object) == -1) {
        printf("PC: menu tyre %s: not found, the race tyre drawn\n", path);
        return NULL;
    }
    TireFile2 *t = (TireFile2 *)file_read_blocking(path, NULL, 0, 1, &vol_device_object);
    if (t == NULL || loc.size < 0x20 || memcmp(t, "GTTR", 4) != 0 || t->tex == NULL) {
        printf("PC: menu tyre %s: not read (or not a tyre file), the race tyre drawn\n", path);
        if (t != NULL)
            free(t);
        return NULL;
    }
    tire_file_relocate(t);
    spare->number = number;
    spare->file = t;
    spare->size = (u32)loc.size;
    spare->users = 1;
    if (size != NULL)
        *size = spare->size;
    printf("PC: menu tyre %s: %u bytes\n", path, (u32)loc.size);
    return t;
}

void pc_menu_tire_addref(TireFile2 *t)
{
    for (Shared &s : g_files)
        if (s.file == t && t != NULL)
            s.users++;
}

void pc_menu_tire_release(TireFile2 *t)
{
    if (t == NULL)
        return;
    for (Shared &s : g_files) {
        if (s.file != t)
            continue;
        if (--s.users <= 0) {
            free(s.file);
            s.file = NULL;
            s.users = 0;
        }
        return;
    }
}
