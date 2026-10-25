/* T511 - GZIP::InflateLoader: InflateBase reading its input from an IOP
 * stream (T523 pdipipe_read).  0x0023E0F0..0x0023E268. */
#include "engine/io/vol_internal.h"
#include "engine/io/T511_inflate_loader.h"

namespace GZIP {

// FUNC 0x0023E0F0 GZIP::InflateLoader::InflateLoader
//   Fills the input buffer once (direct call of fetchInput), starts the bit
//   stream on it, sets the output (setOutputData, inline: outBase and out)
//   and inflates the raw DEFLATE data (inflate_main(false)) right away.
//   Caller: vol_thread_main (T503) with a 64-byte aligned stack buffer.
InflateLoader::InflateLoader(int streamHandle, void *outBuf, void *inBuf, int inBufSize)
    : InflateBase()
{
    stream = streamHandle;
    buf = inBuf;
    bufsize = inBufSize;
    InflateLoader::fetchInput();
    initBitStream(buf, bufsize);
    setOutputData(outBuf);
    inflate_main(false);
}

/* PC (no original): the same with the output buffer's size: inflating
 * stops at outBuf + outSize (stopped = GZ_STOP_OUTPUT_END, the rest of the
 * stream is not read).  Caller: vol_thread_main. */
InflateLoader::InflateLoader(int streamHandle, void *outBuf, u32 outSize, void *inBuf, int inBufSize)
    : InflateBase()
{
    stream = streamHandle;
    buf = inBuf;
    bufsize = inBufSize;
    InflateLoader::fetchInput();
    initBitStream(buf, bufsize);
    setOutputData(outBuf, outSize);
    inflate_main(false);
}

// FUNC 0x0023E1B8 GZIP::InflateLoader::~InflateLoader
//   Resets the vptr and runs ~InflateBase with the same in-charge flags
//   (InflateBase's destructor does the operator delete).
InflateLoader::~InflateLoader()
{
    /* implicit: ~InflateBase (compiler-generated: bases, members, deleting form) */
}

// FUNC 0x0023E1E0 GZIP::InflateLoader::fetchInput
//   The refill hook: reads the next bufsize bytes of the stream into buf and
//   restarts the input at buf (inEnd is left as it is).
void InflateLoader::fetchInput()
{
    pdipipe_read(stream, buf, bufsize);
    inNext = (const u32 *)buf;
}

} /* namespace GZIP */

