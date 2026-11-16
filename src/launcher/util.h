/* util.h - small helpers shared by the launcher's files. */
#pragma once

#include <cstdint>
#include <string>

namespace util {

/* The text of a Win32 error code (FormatMessage), without the final period
 * and line break. */
std::wstring system_error_text(unsigned long code);

/* "3.61 GB", "512 MB", "12 KB" (powers of 1024, as Explorer shows them). */
std::wstring format_size(uint64_t bytes);

/* The folder of the running executable, with a trailing '\'. */
std::wstring exe_folder();

/* `folder` with a trailing '\' (unless empty). */
std::wstring with_slash(std::wstring folder);

/* Creates `folder` and its missing parents; true when it exists afterwards. */
bool make_folders(const std::wstring &folder);

/* UTF-8 <-> UTF-16. */
std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);

/* A folder chosen in the shell's folder dialog over the window `owner' (an
 * HWND; "" when cancelled), which opens in `start' (none: where it was last). */
std::wstring choose_folder(void *owner, const wchar_t *title, const wchar_t *ok,
                           const std::wstring &start = std::wstring());

} // namespace util
