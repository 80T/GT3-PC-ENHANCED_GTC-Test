/* T249 - gt2k_menu::MpegImage.  0x001CBE80..0x001CC5F8.
 *
 * An Item that draws, in order of preference: the current frame of the menu
 * background movie (g_002CDC10), a decoded IPU picture (setIpic), or its
 * normal Image texture.  u0 / v0 / uw / vh select the part of the source
 * that is shown.
 *
 * Data (retail .rodata 0x00336F80..0x003370B8): MpegImage's vtable, then the
 * type names MpegImage, OperatorList, List<OperatorHolder>, OperatorHolder,
 * Cell<OperatorHolder>, Operator: Item's header, then MpegImage's. */
#include "game/menu/framework/T249_mpeg_image.h"           /* (Item's operator classes first) */
#include "game/menu/framework/T250_ipic.h"                 /* gt2k_menu::Ipic */
#include "game/menu/framework/T253_menu_bg_movie.h"        /* MenuBgMovie g_002CDC10 */
#include "game/menu/framework/T204_menu_heap.h"            /* menu_heap_current */
#include "game/menu/framework/T257_menu_image.h"           /* gt2k_menu::Image (texSet), Tex1Image */
#include "game/menu/framework/T259_menu_drawable.h"        /* gt2k_menu::Drawable */
#include "engine/gfx/T514_gs_buffer.h"            /* GSBuffer */
#include "engine/pgl/G001_flare.h"                /* gfx_* / gs_* / pgl_* (the GS driver) */
#include "engine/media/native/media_services.h"     /* PC: gt3m_movie_aspect */
#include "game/app/pc_aspect.h"                 /* PC: pc_movie_fit */
#include "game/app/pc_still.h"                  /* PC: the stills */
#include "game/menu/framework/render_base_internal.h"

namespace gt2k_menu {

/* Item(NULL, NULL), then the texture. */
// FUNC 0x001CBE80 gt2k_menu::MpegImage::MpegImage
MpegImage::MpegImage(Tex1Image *tex)
    : Item()
{
    texSet = tex;
    u0     = 0;
    v0     = 0;
    uw     = 1.0f;
    vh     = 1.0f;
    mirror = 0;
    pixels = NULL;
    pcStill = 0;                            /* PC */
}

// FUNC 0x001CBEF0 gt2k_menu::MpegImage::MpegImage
MpegImage::MpegImage(MenuLayoutItem *res, PageLayout *layout)
    : Item(res, layout)
{
    u0     = 0;
    v0     = 0;
    uw     = 1.0f;
    vh     = 1.0f;
    mirror = 0;
    pixels = NULL;
    pcStill = 0;                            /* PC */
}

/* index.txt: gt2k_menu::MpegImage::virtual_0.  Frees the picture. */
// FUNC 0x001CBF48 gt2k_menu::MpegImage::~MpegImage
MpegImage::~MpegImage()
{
    void *old = pixels;
    if (old != NULL) {
        pixels = NULL;
        menu_heap_free(old);                /* PC: by the block's address */
    }
    /* then Item::~Item (flags 0); the delete is Object::operator delete */
    /* implicit: ~Item (base destructor), operator delete (deleting destructor) */
}

/* Decodes pic into a new menu-heap buffer (NULL: no picture). */
// FUNC 0x001CBFC8 gt2k_menu::MpegImage::setIpic
void MpegImage::setIpic(Ipic *pic)
{
    pcStill = 0;                            /* PC */
    void *old = pixels;
    if (old != NULL) {
        pixels = NULL;
        menu_heap_free(old);                /* PC: by the block's address */
    }
    if (pic == NULL) {
        pixels = NULL;
        return;
    }
    /* PC (ROADMAP 18): whole 16 x 16 blocks (what the decode writes and
     * drawIpic reads; pic->size is width * height * 4), never full */
    u32 bytes = (u32)(((pic->width + PIXEL_BLOCK - 1) / PIXEL_BLOCK) *
                      ((pic->height + PIXEL_BLOCK - 1) / PIXEL_BLOCK) * PIXEL_BLOCK_SIZE);
    if (bytes < (u32)pic->size)
        bytes = (u32)pic->size;
    void *buf = menu_heap_allocate(bytes, 0x40);
    if (buf == NULL) {                      /* PC: no memory at all: no picture (retail decoded into 0) */
        printf("PC: MpegImage: no 0x%X bytes for a %d x %d picture\n", bytes, pic->width,
               pic->height);
        return;
    }
    pic->decode(buf, bytes);                /* PC: with its size */
    pixels    = buf;
    picWidth  = pic->width;
    picHeight = pic->height;
}

/* PC (no original): see T249_mpeg_image.h. */
int MpegImage::setStill(int slot, const char *name)
{
    setIpic(NULL);
    if (pc_still_load(slot, name) == 0)
        return 0;
    pcStill = slot + 1;
    return 1;
}

/* The texture coordinates of the movie frame: its buffer is the frame width
 * rounded up to 64, the texture texW x texH.
 * PC: an interlaced movie's field moved onto its own lines
 * (MenuBgMovie::fieldOffset: progressive, no bob between the fields). */
// FUNC 0x001CC068 gt2k_menu::MpegImage::movieUV
void MpegImage::movieUV(PrimUV *uv)
{
    MenuBgMovie *movie = &g_002CDC10;
    f32 sv = (f32)movie->buf.height / (f32)movie->texH;
    f32 su = (f32)gs_buffer_width(movie->buf.width) / (f32)movie->texW;
    f32 dv = movie->fieldOffset() / (f32)movie->texH;  /* PC */
    if (movie->gpuFrame() != 0) {           /* PC: its picture fills its texture */
        su = 1.0f;
        sv = 1.0f;
        dv = 0.0f;
    }

    uv[0].u = su * u0;
    uv[0].v = sv * v0 + dv;
    uv[1].u = su * (u0 + uw);
    uv[1].v = sv * v0 + dv;
    uv[2].u = su * u0;
    uv[2].v = sv * (v0 + vh) + dv;
    uv[3].u = su * (u0 + uw);
    uv[3].v = sv * (v0 + vh) + dv;
}

/* The texture coordinates of a w x h picture in its power-of-2 texture
 * (at most 0x800). */
// FUNC 0x001CC140 gt2k_menu::MpegImage::ipicUV
void MpegImage::ipicUV(PrimUV *uv, int w, int h)
{
    int texW = 1;
    while (texW < 0x401 && texW < w)
        texW <<= 1;
    f32 su = (f32)w / (f32)texW;

    int texH = 1;
    while (texH < 0x401 && texH < h)
        texH <<= 1;
    f32 sv = (f32)h / (f32)texH;

    uv[0].u = su * u0;
    uv[0].v = sv * v0;
    uv[1].u = su * (u0 + uw);
    uv[1].v = sv * v0;
    uv[2].u = su * u0;
    uv[2].v = sv * (v0 + vh);
    uv[3].u = su * (u0 + uw);
    uv[3].v = sv * (v0 + vh);
}

/* Slot 1, the update (index.txt: gt2k_menu::MpegImage::virtual_1): Item's. */
// FUNC 0x001CC268 gt2k_menu::MpegImage::virtual_1
void MpegImage::virtual_1()
{
    Item::virtual_1();
}

/* Uploads the picture (16 x 16 blocks of 0x400 bytes, in rows of blocks) to
 * a temporary GSBuffer and draws it as a textured quad (bilinear). */
// FUNC 0x001CC288 gt2k_menu::MpegImage::drawIpic
void MpegImage::drawIpic()
{
    GSBuffer tex;                           /* GSBuffer(); ~GSBuffer at the end */
    tex.width  = picWidth;
    tex.height = picHeight;
    tex.psm    = PSM_CT32;
    tex.allocVram();

    const u8 *block = (const u8 *)pixels;
    for (int by = 0; by < picHeight; by += PIXEL_BLOCK) {
        for (int bx = 0; bx < picWidth; bx += PIXEL_BLOCK) {
            /* psm read first (retail loads it before addr; read in the
             * call it comes after, 99.1%) */
            int psm = tex.psm;
            gs_upload_image(tex.addr, psm, gs_buffer_width(tex.width), bx, by,
                            PIXEL_BLOCK, PIXEL_BLOCK, block);
            block += PIXEL_BLOCK_SIZE;
        }
    }
    gfx_texflush();
    tex.setTexture();
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);

    u32 *colors = (u32 *)gfx_alloc_tail(1);
    getColors(colors);
    PrimVertex *verts = (PrimVertex *)gfx_alloc_tail(3);
    setRectVertices(verts, x.value, y.value, w, h);
    PrimUV *uv = (PrimUV *)gfx_alloc_tail(2);
    ipicUV(uv, picWidth, picHeight);
    pgl_draw_strip(verts, NULL, colors, uv, 4);
    tex.freeVram();
}

/* PC (no original): the movie frame as virtual_2 drew it, qw x qh about the
 * item's centre, its colours masked (the alpha alone: black). */
void MpegImage::drawMovie(f32 qw, f32 qh, u32 colorMask)
{
    u32 *colors = (u32 *)gfx_alloc_tail(1);
    getColors(colors);
    for (int i = 0; i < 4; i++)
        colors[i] &= colorMask;
    PrimVertex *verts = (PrimVertex *)gfx_alloc_tail(3);
    if (mirror != 0)
        F_001D01A0(verts, x.value, y.value, qw, qh);
    else
        setRectVertices(verts, x.value, y.value, qw, qh);
    PrimUV *uv = (PrimUV *)gfx_alloc_tail(2);
    movieUV(uv);
    pgl_draw_strip(verts, NULL, colors, uv, 4);
}

/* PC (no original): the still, its host image set as uploadGpu (T253) sets the
 * movie's and drawn as drawMovie: qw x qh about the item's centre, its colours
 * masked.  Its picture fills the TEX0's square, so u0 .. vh are the picture's
 * whatever its size. */
void MpegImage::drawStill(f32 qw, f32 qh, u32 colorMask)
{
    gs_flush_dirty_regs();
    gfx_texflush();
    gfx_set_texture_buffer(0, pc_still_tbp(pcStill - 1) << 6, PSM_CT32, 0);
    gfx_set_texture_size(PC_STILL_SIDE, PC_STILL_SIDE);
    gfx_set_texture_region(PGL_REGION_U, 0, PC_STILL_SIDE - 1);
    gfx_set_texture_region(PGL_REGION_V, 0, PC_STILL_SIDE - 1);
    gfx_tex_param(PGL_TEXPARAM_WMS, PGL_WRAP_CLAMP);
    gfx_tex_param(PGL_TEXPARAM_WMT, PGL_WRAP_CLAMP);
    gfx_tex_param(PGL_TEXPARAM_TFX, PGL_TFX_MODULATE);
    gfx_tex_param(PGL_TEXPARAM_MXL, 0);
    gfx_tex_param(PGL_TEXPARAM_MMIN, PGL_FILTER_LINEAR);
    gfx_tex_param(PGL_TEXPARAM_MMAG, PGL_FILTER_LINEAR);

    u32 *colors = (u32 *)gfx_alloc_tail(1);
    getColors(colors);
    for (int i = 0; i < 4; i++)
        colors[i] &= colorMask;
    PrimVertex *verts = (PrimVertex *)gfx_alloc_tail(3);
    setRectVertices(verts, x.value, y.value, qw, qh);
    PrimUV *uv = (PrimUV *)gfx_alloc_tail(2);
    uv[0].u = u0;
    uv[0].v = v0;
    uv[1].u = u0 + uw;
    uv[1].v = v0;
    uv[2].u = u0;
    uv[2].v = v0 + vh;
    uv[3].u = u0 + uw;
    uv[3].v = v0 + vh;
    pgl_draw_strip(verts, NULL, colors, uv, 4);
}

/* Slot 2, the draw (index.txt: gt2k_menu::MpegImage::virtual_2): the movie
 * frame (if the movie has one), else the picture, else the texture.
 * PC: the frame in its own shape ([Display] MovieAspect, pc_movie_fit), the
 * rest of the item black; a movie's first picture as its still the same, so
 * that the movie takes over without a jump. */
// FUNC 0x001CC448 gt2k_menu::MpegImage::virtual_2
void MpegImage::virtual_2()
{
    if (visible == 0)
        return;

    if (g_002CDC10.lastFrame != NULL) {
        Drawable::virtual_2();
        g_002CDC10.upload();
        f32 movieW = w, movieH = h;
        if (pc_movie_fit((f32)gt3m_movie_aspect() * uw / vh, &movieW, &movieH) != 0)
            drawMovie(w, h, 0xFF000000u);
        drawMovie(movieW, movieH, 0xFFFFFFFFu);
        g_002CDC10.release();
    } else if (pcStill != 0) {              /* PC */
        f32 stillW = w, stillH = h;
        const f32 aspect = pc_still_aspect(pcStill - 1);
        if (aspect > 0.0f && pc_movie_fit(aspect * uw / vh, &stillW, &stillH) != 0)
            drawStill(w, h, 0xFF000000u);
        drawStill(stillW, stillH, 0xFFFFFFFFu);
    } else if (pixels != NULL) {
        drawIpic();
    } else if (texSet != NULL) {
        Image::virtual_2();
    }
}

// FUNC 0x001CC588 gt2k_menu::MpegImage::setMirror
/* inline in game/menu/framework/T249_mpeg_image.h; egcs emits this copy (#pragma implementation) */

// FUNC 0x001CC590 gt2k_menu::MpegImage::setUVRect
/* inline in game/menu/framework/T249_mpeg_image.h; egcs emits this copy (#pragma implementation) */

} /* namespace gt2k_menu */

