/* mp4_token.h - an MP4 movie's units on the PSS path (GT3 PC; docs/movies.md
 * "MP4 movies").
 *
 * mpegif (T529) feeds the player 4 KB units, each one video PES packet; an
 * MP4 movie keeps that path - its feed, end marker, loops, preloaded copy,
 * stops and switches as they are - with one token per frame in the PES
 * packet's payload: frame `index' (at the game's 29.97 a second) of the
 * movie at `path'.  The service (mpg_service.cpp) makes them, the player's
 * decoder (media_services.cpp) takes the frames they name from an
 * Mp4FrameWorker (mp4_frames.h). */
#pragma once

#include <cstdint>
#include <cstring>

namespace gt3media {

constexpr char kMp4TokenMagic[8] = {'G', 'T', '3', 'M', 'P', '4', 0, 1};
enum { MP4_TOKEN_LOOP = 1 };                /* frame 0 follows the last */

struct Mp4Token {
    char magic[8];
    uint32_t index;                         /* the frame */
    uint32_t frames;                        /* the movie's (the last: frames - 1) */
    uint32_t flags;                         /* MP4_TOKEN_* */
    char path[1024];                        /* the Windows path, terminated */
};

inline bool is_mp4_token(const void *data, int size)
{
    return size >= (int)sizeof(Mp4Token) && std::memcmp(data, kMp4TokenMagic, sizeof(kMp4TokenMagic)) == 0;
}

} // namespace gt3media
