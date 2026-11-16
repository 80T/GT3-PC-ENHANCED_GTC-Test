/* pc_image_pack.h - PC: an image pack read from its folder of PNG files
 * (docs/graphics.md "Image packs as PNG folders").  No original: new PC code.
 *
 * User request (2026-11-08): ".imgs conversion to folder, and texture
 * conversion to standard format.  I suggest .png".  The launcher writes each
 * <pack>.imgs of the data folder as the folder <pack>/ with a <name>.png for
 * each image (launcher/image_packs.h).  image_pack_load (engine/io T506)
 * asks here first: "race/US/display.imgs" is then the folder race/US/display/
 * when it holds PNG files - each <name>.png the image <name>, a mod's folder's
 * too, file by file (host_file_list: a mod's <name>.png in place of the data
 * folder's, its new ones added) - built into a pack in the original's file
 * form (engine/pgl/native/gs_imgs.h: each picture a texture with the pack
 * images' settings, T4 / T8 for 16 / 256 colours or fewer, else CT32) for
 * image_pack_relocate, as a loaded .imgs file is.  A pack file still there
 * (one the launcher could not convert, or a mod's) gives the images the
 * folder lacks.  A picture the game cannot take (no PNG, larger than 1024
 * pixels a side, a name longer than 59 characters) is left out and said in
 * the log; the HUD then draws the pack's "00no_texture" for it, as for any
 * name a pack lacks. */
#ifndef GAME_APP_PC_IMAGE_PACK_H
#define GAME_APP_PC_IMAGE_PACK_H

/* The pack file's bytes built from the folder of `path' (a pack's path:
 * "race/US/display.imgs"), in a block of the game's heap (free() releases
 * it, as a loaded pack); NULL when the folder holds no PNG file (the pack
 * file is read as before).  On a game thread. */
void *pc_image_pack_folder(const char *path);

/* PC (2026-11-08, user: "bigger / smaller texture dimensions [should not]
 * change the size of HUD elements"): the size image t is drawn at, in the
 * HUD's pixels - for a pack image built here its draw size (the folder's
 * sizes.txt: its original's, whatever the PNG's size; gs_imgs.h
 * kImgsDrawMagic), else its texture's (CLAMP MAXU / MAXV + 1, the
 * original's rule).  The HUD's draws take their on-screen size from this and
 * their texture coordinates from the texture (race_draw_image,
 * race_display_draw_tex, the round meters' scale - their needle -, the
 * accel / brake gauge, the steering sign, the place mark, the replay mode
 * labels: docs/graphics.md 4.6). */
struct Tex1Image;
void pc_image_draw_size(const struct Tex1Image *t, int *w, int *h);

#endif /* GAME_APP_PC_IMAGE_PACK_H */
