/* ui_text_check.h - when a translation may stand for its English
 * (platform/ui_text.h): the one rule the game, the launcher (ui_text.cpp:
 * a translation that breaks it is not taken, the English shows) and the
 * build's tools/lang_sync (it reports the translation) share.  PC
 * (2026-11-16).  No Windows headers. */
#pragma once

#include <ctype.h>
#include <string.h>
#include <string>

namespace ui_text_check {

/* A string's printf conversions, what matters to the arguments they read:
 * "*" for a width or precision read from them, the size and the type ("d,",
 * "ls,"). */
inline std::string conversions(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%')
            continue;
        size_t j = i + 1;
        if (j < s.size() && s[j] == '%') {
            i = j;
            continue;
        }
        while (j < s.size() && s[j] != '\0' && strchr("-+ #0", s[j]) != nullptr)
            j++;
        for (; j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '*'); j++)
            if (s[j] == '*')
                out += '*';
        if (j < s.size() && s[j] == '.')
            for (j++; j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '*'); j++)
                if (s[j] == '*')
                    out += '*';
        for (; j < s.size() && s[j] != '\0' && strchr("hlLzjtwI", s[j]) != nullptr; j++) {
            out += s[j];
            for (; s[j] == 'I' && j + 1 < s.size() && isdigit((unsigned char)s[j + 1]); j++)
                out += s[j + 1];
        }
        if (j < s.size())
            out += s[j];
        out += ',';
        i = j;
    }
    return out;
}

/* A string with a '%' that starts no valid conversion (strictly: no ' '
 * flag; a known type) and is not "%%" is no printf format - "75% OF ..." in a
 * help text: its English could not be one, so neither is its translation,
 * whose own '%'s then need no checking.  (After the German catalogue: its
 * translations had to keep the letter after every %.) */
inline bool plain(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '%')
            continue;
        size_t j = i + 1;
        if (j < s.size() && s[j] == '%') {
            i = j;
            continue;
        }
        while (j < s.size() && s[j] != '\0' && strchr("-+#0", s[j]) != nullptr)
            j++;
        while (j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '*'))
            j++;
        if (j < s.size() && s[j] == '.')
            for (j++; j < s.size() && (isdigit((unsigned char)s[j]) || s[j] == '*'); j++) {
            }
        while (j < s.size() && s[j] != '\0' && strchr("hlLzjtwI", s[j]) != nullptr)
            for (j++; j < s.size() && isdigit((unsigned char)s[j]) && (s[j - 1] == 'I' || isdigit((unsigned char)s[j - 1]));
                 j++) {
            }
        if (j >= s.size() || s[j] == '\0' || strchr("diouxXeEfFgGaAcspnSC", s[j]) == nullptr)
            return true;
        i = j;
    }
    return false;
}

/* The translation may stand for the English: the same conversions in the
 * same order, unless the English is no format at all. */
inline bool conversions_match(const std::string &english, const std::string &translation)
{
    return plain(english) || conversions(english) == conversions(translation);
}

} // namespace ui_text_check
