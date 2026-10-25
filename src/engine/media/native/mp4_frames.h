/* mp4_frames.h - an MP4 movie's frames for the G002 player's queue (GT3
 * PC; docs/movies.md "MP4 movies").
 *
 * A native thread of its own (not a game thread: the decoding, 5 .. 20 ms a
 * 1080p picture, must not hold the game's threads) decodes the movie
 * (mp4_movie.h) ahead into a few ready frames.  The game takes a frame
 * every other tick (MenuBgMovie::update: frame n at n x 2e9 / 59933 us) and
 * shows its upper half on the first tick, its lower half on the next (a
 * PSS's two fields), so each frame is two pictures stacked: the movie at
 * frame n's time above, half a frame (1 / 59.94 s) later below - up to
 * 59.94 pictures a second.  Each picture is the source picture shown at its
 * time (a 59.94 / 60 fps movie gives every one, a 30 fps one each twice, a
 * 24 fps one some three times), scaled into the slot (the stream's width x
 * height) with its aspect kept (the slot shown at 4:3 for 640 x 448: the
 * PS2's pixels; black bars where the aspects differ; an area average down,
 * linear up).  The frame, width x 2 height, is in the queue's layout: 16 x
 * 16 blocks of RGBA (alpha 0x80) in columns of blocks, as the IPU's CSC
 * wrote them.  A movie that does not decode gives black frames (logged), so
 * its stream still runs to its end. */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gt3media {

class Mp4FrameWorker {
public:
    /* path: the Windows path; width x height: a picture's (the frame holds
     * two); frames: the movie's frame count (mp4_frame_count); loop: frame
     * 0 follows the last (prepared ahead). */
    Mp4FrameWorker(const std::string &path, int width, int height, int frames, bool loop);
    ~Mp4FrameWorker();
    Mp4FrameWorker(const Mp4FrameWorker &) = delete;
    Mp4FrameWorker &operator=(const Mp4FrameWorker &) = delete;

    /* Frame `index' into dst (width x 2 height x 4 bytes): true; false while
     * it is not ready (a moment: ask again).  A frame out of the sequence
     * (a jump) restarts the decoding there. */
    bool take(int index, uint8_t *dst);

    const std::string &path() const { return path_; }

private:
    struct Impl;
    std::string path_;
    std::unique_ptr<Impl> impl_;
};

/* Frame n's time in 100 ns (n x 2e9 / 59933 us); its lower picture's is
 * half a frame later. */
inline int64_t mp4_frame_time(int64_t n)
{
    return n * 20000000000LL / 59933;
}

/* The frames of a movie of `duration' (100 ns) at the game's rate. */
inline int mp4_frame_count(int64_t duration)
{
    return duration > 0 ? (int)((duration * 59933 + 20000000000LL - 1) / 20000000000LL) : 0;
}

} // namespace gt3media
