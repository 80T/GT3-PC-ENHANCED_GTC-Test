/* menulayout - the menu layout files (.pmb) as source (menu_layout.h),
 * offline.  The game reads the .pmb files; this is the tool's only.
 *
 *   menulayout decompile FILE.pmb OUT.menu OUT.tex   the source and the texture file
 *   menulayout compile FILE.menu FILE.tex OUT.pmb     the layout file back
 *   menulayout roundtrip FILE.pmb...                  each decompiled, compiled, compared
 *                                                     (a line a file refused, a summary)
 *   menulayout roundtrip @LIST                        the files listed in LIST, a line each
 *
 * The encoding: euc-jp for a file in a folder named JP, else gt3. */
#include "menu_layout.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool read_file(const std::string &path, std::vector<uint8_t> *out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->resize(n > 0 ? (size_t)n : 0);
    const bool ok = out->empty() || std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

bool write_file(const std::string &path, const void *data, size_t size)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    return ok;
}

menu_layout::Encoding encoding_of(const std::string &path)
{
    std::string p = path;
    for (char &c : p)
        if (c == '\\')
            c = '/';
    return p.find("/JP/") != std::string::npos ? menu_layout::Encoding::euc_jp : menu_layout::Encoding::gt3;
}

std::string stem_name(const std::string &path)
{
    std::string p = path;
    for (char &c : p)
        if (c == '\\')
            c = '/';
    const size_t m = p.find("/menu/");
    std::string s = m != std::string::npos ? p.substr(m + 1) : p;
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".pmb") == 0)
        s.resize(s.size() - 4);
    return s;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 5 && std::strcmp(argv[1], "decompile") == 0) {
        std::vector<uint8_t> pmb, tex;
        std::string src, why;
        if (!read_file(argv[2], &pmb)) {
            std::fprintf(stderr, "cannot read %s\n", argv[2]);
            return 1;
        }
        if (!menu_layout::decompile(pmb.data(), pmb.size(), encoding_of(argv[2]), stem_name(argv[2]), &src, &tex,
                                    &why)) {
            std::fprintf(stderr, "%s: %s\n", argv[2], why.c_str());
            return 1;
        }
        return write_file(argv[3], src.data(), src.size()) && write_file(argv[4], tex.data(), tex.size()) ? 0 : 1;
    }
    if (argc >= 5 && std::strcmp(argv[1], "compile") == 0) {
        std::vector<uint8_t> src, tex, pmb;
        std::string err;
        if (!read_file(argv[2], &src) || !read_file(argv[3], &tex)) {
            std::fprintf(stderr, "cannot read the source or the texture file\n");
            return 1;
        }
        if (!menu_layout::compile((const char *)src.data(), src.size(), tex.data(), tex.size(), &pmb, &err)) {
            std::fprintf(stderr, "%s: %s\n", argv[2], err.c_str());
            return 1;
        }
        return write_file(argv[4], pmb.data(), pmb.size()) ? 0 : 1;
    }
    if (argc >= 3 && std::strcmp(argv[1], "roundtrip") == 0) {
        std::vector<std::string> files;
        for (int i = 2; i < argc; i++) {
            if (argv[i][0] == '@') {
                std::vector<uint8_t> list;
                if (!read_file(argv[i] + 1, &list))
                    return 1;
                std::string cur;
                for (uint8_t c : list) {
                    if (c == '\n' || c == '\r') {
                        if (!cur.empty())
                            files.push_back(cur);
                        cur.clear();
                    } else {
                        cur += (char)c;
                    }
                }
                if (!cur.empty())
                    files.push_back(cur);
            } else {
                files.push_back(argv[i]);
            }
        }
        size_t same = 0, refused = 0, sourceBytes = 0, layoutBytes = 0, escapes = 0;
        for (const std::string &path : files) {
            std::vector<uint8_t> pmb, tex;
            std::string src, why;
            if (!read_file(path, &pmb)) {
                std::printf("%s: cannot read\n", path.c_str());
                refused++;
                continue;
            }
            if (!menu_layout::decompile(pmb.data(), pmb.size(), encoding_of(path), stem_name(path), &src, &tex,
                                        &why)) {
                std::printf("%s: REFUSED (%zu bytes): %s\n", path.c_str(), pmb.size(), why.c_str());
                refused++;
                continue;
            }
            same++;
            sourceBytes += src.size();
            layoutBytes += pmb.size() - (tex.size() - menu_layout::kTexHeader);
            for (size_t k = 0; k + 1 < src.size(); k++)
                if (src[k] == '\\' && src[k + 1] == 'x')
                    escapes++;
        }
        std::printf("%zu files: %zu identical after decompile + compile, %zu refused; layout parts %zu bytes -> "
                    "source %zu bytes; %zu \\x escapes\n",
                    files.size(), same, refused, layoutBytes, sourceBytes, escapes);
        return 0;
    }
    std::fprintf(stderr, "usage: menulayout decompile FILE.pmb OUT.menu OUT.tex | compile FILE.menu FILE.tex OUT.pmb |"
                         " roundtrip FILE.pmb... | roundtrip @LIST\n");
    return 2;
}
