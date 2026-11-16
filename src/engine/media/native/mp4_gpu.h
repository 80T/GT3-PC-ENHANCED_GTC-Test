/* mp4_gpu.h - an MP4 movie decoded by the GPU, drawn at its own size (GT3 PC;
 * docs/movies.md "MP4 movies").
 *
 * Mp4FrameWorker (mp4_frames.h) brings a movie down to the PS2's slot on the
 * CPU: 640 x 448 at best, and a 4K movie does not keep up (software decoding
 * alone: 8.7 frames a second, measured).  Here Media Foundation decodes on a
 * D3D11 device of its own, on the game's adapter, with the GPU's decoder and
 * video processor: the pictures stay in textures, at the movie's size, and
 * the CPU does next to nothing (the same movie: 216 frames a second).
 *
 * A native thread reads the pictures and copies each one used into one of
 * PGLR_MOVIE_SLOTS shared textures (keyed mutex, key 0), which the renderer
 * opens on its own device (pgl_movie.cpp: pglr_movie_*).  The device is the
 * movie's alone: Media Foundation's calls cannot touch the game's state.
 *
 * The game's side stays the PSS path's, tokens and all (mp4_token.h): frame n
 * of the player's queue is two pictures 1 / 59.94 s apart, an Mp4GpuFrame in
 * place of its pixels, and MenuBgMovie shows them half by half.  A slot comes
 * back once the game has taken a later frame (mp4_gpu_keep_from).
 *
 * open() fails (no adapter, no hardware decoder for this movie: nothing but
 * system memory comes out) and the movie goes the CPU's way. */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gt3media {

constexpr char kMp4GpuMagic[8] = {'G', 'T', '3', 'G', 'P', 'U', 0, 1};

/* A frame in the player's queue.  The magic cannot be a picture's: the first
 * pixel's alpha is at byte 3, always 0x80. */
struct Mp4GpuFrame {
    char magic[8];
    int32_t slot[2];                        /* the pictures' textures (upper, lower half; -1: none) */
    uint32_t seq[2];                        /* the pictures' numbers, never reused */
};

class Mp4GpuFrames {
public:
    /* The movie opened and its first picture decoded by the GPU, or null.
     * path: the Windows path; frames: mp4_frame_count; loop: frame 0 follows
     * the last.  Only the calling game thread waits (sched::run_outside). */
    static std::unique_ptr<Mp4GpuFrames> open(const std::string &path, int frames, bool loop);
    ~Mp4GpuFrames();
    Mp4GpuFrames(const Mp4GpuFrames &) = delete;
    Mp4GpuFrames &operator=(const Mp4GpuFrames &) = delete;

    /* Frame `index' into out: true; false while it is not ready (a moment:
     * ask again).  A frame out of the sequence (a jump) restarts there. */
    bool take(int index, Mp4GpuFrame *out);

    const std::string &path() const { return path_; }
    int width() const;
    int height() const;
    /* The picture's shape as shown: width x pixel aspect / height. */
    double aspect() const;

private:
    Mp4GpuFrames();
    struct Impl;
    std::string path_;
    std::unique_ptr<Impl> impl_;
};

/* The pictures before number seq are not needed any more (any thread). */
void mp4_gpu_keep_from(uint32_t seq);

} // namespace gt3media
