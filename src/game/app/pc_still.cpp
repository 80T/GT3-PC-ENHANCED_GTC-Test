/* pc_still.cpp - PC: the arcade menus' stills (see pc_still.h). */
#include "libc.h"
#include "game/app/pc_still.h"
#include "game/app/pc_data_file.h"
#include "engine/media/native/still_cache.h"
#include "engine/pgl/native/pgl_bridge.h"
#include "platform/png_image.h"
#include "platform/scheduler.h"

#include <string>
#include <vector>

namespace {

struct Still {
    PglrHostImage image;
    uint64_t tex0;
    std::vector<uint8_t> pixels;
    f32 aspect;
};

Still s_stills[PC_STILLS];

/* The PNG's decoding, outside the game's CPU (a big picture takes a while). */
struct Decode {
    gt3media::StillFile file;
    std::vector<uint8_t> *rgba;
    int w, h;
    double pixelAspect;
    bool ok;
};

void decode_png(void *arg)
{
    Decode &d = *static_cast<Decode *>(arg);
    d.ok = png_image::decode(d.file.data, d.file.size, d.rgba, &d.w, &d.h, &d.pixelAspect);
}

/* The image of the slot, registered the first time; false when no more fit. */
bool still_image(Still &st)
{
    if (st.tex0 != 0)
        return true;
    memset(&st.image, 0, sizeof(st.image));
    st.image.id = -1;
    st.tex0 = pglr_host_image_add(&st.image, 10, 10);       /* (PC_STILL_SIDE: 2^10) */
    return st.tex0 != 0;
}

} // namespace

int pc_still_load(int slot, const char *name)
{
    Still &st = s_stills[slot];
    std::string read;
    Decode d;
    if (!gt3media::still_cache_find(name, &d.file)) {
        const std::string path = std::string("stream/course/") + name + ".png";
        if (!pc_data_file_read(path, &read))
            return 0;
        d.file.data = reinterpret_cast<const uint8_t *>(read.data());
        d.file.size = read.size();
        d.file.movie = false;
    }
    std::vector<uint8_t> rgba;
    d.rgba = &rgba;
    d.w = d.h = 0;
    d.pixelAspect = 1.0;
    d.ok = false;
    sched::run_outside(decode_png, &d);
    if (!d.ok) {
        printf("PC: the still %s is not a PNG the game can read\n", name);
        return 0;
    }
    if (!still_image(st))
        return 0;
    /* GS units: alpha 0x80 is 1.0 */
    for (size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = (uint8_t)((rgba[i] * 128u + 127u) / 255u);
    PglrHostImage &im = st.image;
    pglr_host_image_lock(&im);
    st.pixels.swap(rgba);
    im.gsW = PC_STILL_SIDE;
    im.gsH = PC_STILL_SIDE;
    im.w = d.w;
    im.h = d.h;
    im.pixels = reinterpret_cast<uint32_t *>(st.pixels.data());
    im.generation++;
    im.dirtyY0 = im.dirtyY1 = 0;
    pglr_host_image_unlock(&im);
    st.aspect = d.file.movie ? (f32)((double)d.w * d.pixelAspect / d.h) : 0.0f;
    return 1;
}

u32 pc_still_tbp(int slot)
{
    return (u32)(s_stills[slot].tex0 & 0x3FFF);
}

f32 pc_still_aspect(int slot)
{
    return s_stills[slot].aspect;
}
