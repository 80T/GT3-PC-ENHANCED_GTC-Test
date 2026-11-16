/* pc_image_pack.cpp - PC: an image pack from its folder of PNG files
 * (pc_image_pack.h). */
#include "libc.h"
#include "game/app/pc_image_pack.h"
#include "game/app/pc_data_file.h"
#include "engine/pgl/G001_flare.h"           /* Tex1Image, GsTexRegs (pc_image_draw_size) */
#include "engine/pgl/native/gs_imgs.h"
#include "platform/host_file.h"
#include "platform/png_image.h"
#include "platform/scheduler.h"

#include <chrono>
#include <string>
#include <vector>

namespace {

struct Picture {
    std::string name;                       /* the file's name without .png */
    std::string file;                       /* its bytes */
};

/* The folder's PNG files' names (host_file_list). */
void add_png(const char *name, void *arg)
{
    const size_t n = strlen(name);
    if (n > 4 && _stricmp(name + n - 4, ".png") == 0)
        static_cast<std::vector<std::string> *>(arg)->push_back(name);
}

/* The building, outside the game's CPU (the PNGs decoded, the textures
 * made). */
struct Build {
    std::vector<Picture> *pictures;
    const std::string *packFile;            /* the .imgs file's bytes ("" none) */
    const std::string *sizesFile;           /* sizes.txt's text ("" none) */
    std::vector<uint8_t> out;
    std::vector<std::string> notes;         /* a line each for the log */
    int fromPng = 0, fromPack = 0;
    int resized = 0;                        /* pictures drawn at another size than their PNG's */
    bool ok = false;
};

void build(void *arg)
{
    Build &b = *static_cast<Build *>(arg);
    std::vector<gstex::ImgsImage> images;
    std::string why;
    if (!b.packFile->empty()) {
        if (gstex::imgs_read(reinterpret_cast<const uint8_t *>(b.packFile->data()), b.packFile->size(), images, &why))
            b.fromPack = (int)images.size();
        else
            b.notes.push_back("the pack file is not read: " + why);
    }
    png_image::Session png;                 /* (COM and the imaging factory once for all the pictures) */
    for (const Picture &p : *b.pictures) {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!png.decode(p.file.data(), p.file.size(), &rgba, &w, &h)) {
            b.notes.push_back(p.name + ".png is left out: not a PNG the game can read");
            continue;
        }
        if (w > (int)gstex::kImgsSideMax || h > (int)gstex::kImgsSideMax) {
            b.notes.push_back(p.name + ".png is left out: " + std::to_string(w) + " x " + std::to_string(h) +
                              " is larger than " + std::to_string(gstex::kImgsSideMax) + " pixels a side");
            continue;
        }
        if (p.name.empty() || p.name.size() > gstex::kImgsNameMax) {
            b.notes.push_back(p.name + ".png is left out: its name is longer than " +
                              std::to_string(gstex::kImgsNameMax) + " characters");
            continue;
        }
        gstex::ImgsImage img;
        img.name = p.name;
        img.w = (uint32_t)w;
        img.h = (uint32_t)h;
        img.rgba.resize((size_t)w * h);
        for (size_t i = 0; i < img.rgba.size(); i++)
            img.rgba[i] = (uint32_t)rgba[i * 4] | ((uint32_t)rgba[i * 4 + 1] << 8) |
                          ((uint32_t)rgba[i * 4 + 2] << 16) | (gstex::imgs_alpha_from_png(rgba[i * 4 + 3]) << 24);
        /* in place of the pack file's image of that name (its case kept: the
         * HUD asks by the original's), else added */
        bool replaced = false;
        for (gstex::ImgsImage &x : images)
            if (_stricmp(x.name.c_str(), img.name.c_str()) == 0) {
                img.name = x.name;
                img.drawW = x.w;            /* (drawn at the pack image's size unless sizes.txt says) */
                img.drawH = x.h;
                x = std::move(img);
                replaced = true;
                b.fromPack--;
                break;
            }
        if (!replaced)
            images.push_back(std::move(img));
        b.fromPng++;
    }
    /* PC (2026-11-08): the draw sizes - sizes.txt's (its original's, which
     * the launcher wrote), else a pack file image's own size (its pixels are
     * the original's), else the PNG's - so a picture of another size draws
     * the HUD element at its size (user: "bigger / smaller texture
     * dimensions [should not] change the size of HUD elements") */
    const std::vector<gstex::ImgsSize> sizes = gstex::imgs_sizes_parse(*b.sizesFile);
    for (gstex::ImgsImage &img : images)
        for (const gstex::ImgsSize &s : sizes)
            if (_stricmp(s.name.c_str(), img.name.c_str()) == 0) {
                img.drawW = s.w;
                img.drawH = s.h;
                if (s.w != img.w || s.h != img.h)
                    b.resized++;
                break;
            }
    b.ok = gstex::imgs_build(std::move(images), b.out, &why);
    if (!b.ok)
        b.notes.push_back("the pack could not be built: " + why);
}

} // namespace

void *pc_image_pack_folder(const char *path)
{
    const std::string pack = path != NULL ? path : "";
    if (pack.size() <= 5 || _stricmp(pack.c_str() + pack.size() - 5, ".imgs") != 0)
        return NULL;
    const std::string folder = pack.substr(0, pack.size() - 5);
    std::vector<std::string> names;
    host_file_list(("host:data/" + folder).c_str(), add_png, &names);
    if (names.empty())
        return NULL;
    const auto start = std::chrono::steady_clock::now();
    std::vector<Picture> pictures;
    for (const std::string &n : names) {
        Picture p;
        p.name = n.substr(0, n.size() - 4);
        if (pc_data_file_read(folder + "/" + n, &p.file))
            pictures.push_back(std::move(p));
        else
            printf("PC: image pack %s: %s could not be read\n", folder.c_str(), n.c_str());
    }
    std::string packFile, sizesFile;
    pc_data_file_read(pack, &packFile);     /* (a quiet probe: none, "") */
    pc_data_file_read(folder + "/" + gstex::kImgsSizesFile, &sizesFile);
    Build b;
    b.pictures = &pictures;
    b.packFile = &packFile;
    b.sizesFile = &sizesFile;
    sched::run_outside(build, &b);
    for (const std::string &note : b.notes)
        printf("PC: image pack %s: %s\n", folder.c_str(), note.c_str());
    if (!b.ok)
        return NULL;
    void *block = memalign(16, b.out.size());
    if (block == NULL) {
        printf("PC: image pack %s: no memory for %u bytes\n", folder.c_str(), (unsigned)b.out.size());
        return NULL;
    }
    memcpy(block, b.out.data(), b.out.size());
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    printf("PC: image pack %s: %d pictures from %s/*.png", folder.c_str(), b.fromPng, folder.c_str());
    if (b.fromPack > 0)
        printf(", %d from %s", b.fromPack, pack.c_str());
    if (b.resized > 0)
        printf(", %d drawn at their %s size, not their PNG's", b.resized, gstex::kImgsSizesFile);
    if (sizesFile.empty())
        printf(", no %s (each picture drawn at its PNG's size)", gstex::kImgsSizesFile);
    printf(" (%lld ms)\n", ms);
    return block;
}

void pc_image_draw_size(const Tex1Image *t, int *w, int *h)
{
    uint32_t magic;
    memcpy(&magic, t->unk_28 + 4, 4);
    if (magic == gstex::kImgsDrawMagic) {
        uint16_t dw, dh;
        memcpy(&dw, t->unk_28, 2);
        memcpy(&dh, t->unk_28 + 2, 2);
        if (dw != 0 && dh != 0) {
            *w = dw;
            *h = dh;
            return;
        }
    }
    const u64 clamp = t->tex->clamp;
    *w = (int)(((clamp >> 14) & 0x3FF) + 1);
    *h = (int)(((clamp >> 34) & 0x3FF) + 1);
}
