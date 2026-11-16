/* mod_card.h - a folder's card: what a mod folder (mod_manager.h) or a
 * texture folder (texture_manager.h) says of itself with two files at its
 * top, both optional - mod.txt (Title:, Author: or Authors:, Description:,
 * the last one to the file's end) and mod.png (a 1:1 picture) - read for the
 * managers' lists (the title) and for the panel under them (the picture,
 * title, authors and description).
 *
 * PC (2026-11-13): out of mod_manager.cpp as it was, for the texture manager
 * to show the same card. */
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mod_card {

constexpr const wchar_t *kInfoFile = L"mod.txt", *kImageFile = L"mod.png";     /* (at a folder's top) */

/* What a folder says of itself (its mod.txt). */
struct Info {
    std::wstring title, authors, description;
};

/* `p' whole (GetFullPathName), without a final '\' (a drive's root keeps it). */
std::wstring full_path(const std::wstring &p);

/* A folder's mod.txt:
 *     Title: <the mod's name>
 *     Author: <one or more> (or Authors; several lines are joined)
 *     Description: <text, this line and every line after it to the file's end>
 * Names in any case, other lines left out.  With no title: the folder's name. */
Info read_info(const std::wstring &folder);

/* The default picture (the resource IDR_MOD_IMAGE): a template's mod.png, and
 * the panel's for a folder with none. */
bool default_image(const void **data, DWORD *size);

/* `path' (mod.png; "": the default picture) scaled to fit a `box' x `box'
 * square, its shape kept, laid over the window colour, into top-down 32-bit
 * pixels; false when it cannot be read as a picture. */
bool load_image(const std::wstring &path, int box, std::vector<uint32_t> *pixels, int *w, int *h);

/* The picture box (an owner-drawn static): load_image's picture centred in it
 * (empty: none), a sunken edge around it. */
void draw_image(const DRAWITEMSTRUCT *d, const std::vector<uint32_t> &pixels, int w, int h);

} // namespace mod_card
