/* pgl_capture.h - the frame capture (PC, 2026-11-06; docs/renderer.md 10,
 * "The frame capture"): one frame, rendered exactly as every other, recorded
 * in full - every command the executor ran and what it took, every resolved
 * run replayed, every Direct3D draw the submit stage made (what it is, where
 * it came from, why it did not join the draw before it, what was bound for
 * it, what it cost the stage) and what the GPU did for each (its pipeline
 * statistics: the vertices, the primitives rasterized, the pixel shader
 * invocations; its timestamps either side) - written to capture\frame_N\
 * next to the executable (GT3PC_CAPTURE_DIR) as CSV files and a summary,
 * with the frame's targets as pictures.  Armed by the FrameCapture hotkey
 * (F11) or GT3PC_PGL_CAPTURE=N (frame N; racebench --capture N).
 *
 * Unlike the F9 trace it changes nothing: the merged draws, the resolved
 * runs' works and the submit workers stay on - the record is of the frame
 * as the game renders it.  Its own cost falls on the captured frame alone
 * (a few queries a draw; that frame is slower). */
#pragma once

#include <cstdint>

namespace pglr {
namespace cap {

/* What a Direct3D call recorded is */
enum Kind : uint8_t {
    K_VU1 = 0,      /* an indexed VU1 draw of one part */
    K_VU1_MERGED,   /* a merged VU1 draw (several parts, their textures in slots) */
    K_GS,           /* a GS batch (sprites, strips: pgl's 2D) */
    K_MIRROR,       /* an alpha mirror pass (the draw before it drawn again for its alpha bit) */
    K_PASS,         /* a full-screen pass (a sync, a resolve, a filter) */
    K_COPY,         /* a copy between textures (a target's copy refreshed) */
    K_RESOLVE,      /* a multisampled target resolved */
    K_CLEAR,        /* a clear */
    K_PRESENT,      /* the picture onto the screen */
    K_FLARE,        /* the flare queries' batch */
    K_COUNT
};
/* How a C = Ad form was drawn */
enum AdForm : uint8_t { AD_NONE = 0, AD_2INST, AD_PAIR1, AD_PAIR2, AD_TWICE1, AD_TWICE2, AD_SHADER, AD_DOUBLE, AD_COUNT };
/* Why a VU1 part began a draw of its own instead of joining the one before
 * (merge_join's reasons; 0: the first draw, or no draw to join) */
enum MergeWhy : uint8_t {
    MW_FIRST = 0, MW_NONE, MW_STATE, MW_VS, MW_BUF, MW_INDEX, MW_BATCH, MW_REC, MW_PS, MW_SLOTS, MW_TARGET_TEX,
    MW_AD_INST, MW_CB, MW_COUNT
};

struct Draw {
    uint32_t id = 0;                        /* the record (allocation order on the submit side) */
    uint32_t order = ~0u;                   /* its place in the D3D thread's order (set as it runs) */
    uint8_t kind = K_VU1, adForm = AD_NONE, program = 255, cause = 0;
    uint8_t prim = 255;                     /* a GS batch's primitive type */
    uint8_t blendSel = 0, writeMask = 0, zTest = 0, zWrite = 0, zFunc = 0, date = 0, datm = 0;
    uint8_t stencilOp = 0, stencilRef = 0, mirrorPass = 0, fold = 0, flat = 0, slots = 0, mergeWhy = 0, thread = 0;
    uint8_t depthFrame = 0, tme = 0, texTarget = 0;
    uint8_t atst = 0, aref = 0, agate = 0;  /* the alpha test (ATST as gs::Compare, AREF) and the pass's gate (AlphaGate) */
    uint16_t parts = 1, instances = 1;
    uint16_t part = 0xFFFF;                 /* the first part's index in its run */
    int32_t run = -1;                       /* the resolved run (its capture number), -1 none */
    uint32_t count = 0;                     /* indices (a VU1 draw) or vertices (a GS batch) */
    uint32_t target = 0;                    /* the colour target's base block */
    uint16_t targetW = 0, targetH = 0;      /* its native size */
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; /* the draw's area (native pixels), or its scissor */
    uint32_t binds = 0;                     /* D3DB_* bits: the bindings issued for it */
    uint32_t cpuNs = 0;                     /* the submit stage's time since its previous record (this thread) */
    const char *note = nullptr;             /* a pass's shader, a copy's reason */
    /* the GPU's (read after the frame) */
    double gpuUs = -1.0;                    /* the timestamps either side of it */
    uint64_t iaVerts = 0, iaPrims = 0, vsInv = 0, rastPrims = 0, psInv = 0;
};

/* The capture of this frame is on (the executor's view; the stage's threads
 * read it too - it changes only between frames) */
extern bool g_on;
inline bool on() { return g_on; }

/* Armed: the next frame is captured (the hotkey, GT3PC_PGL_CAPTURE) */
void arm();
/* exec_begin_frame: the capture begins if armed (frame: its number); the
 * pending capture's GPU queries read and its files written once they are */
void begin_frame(uint64_t frame);
void poll();
/* exec_end_frame: the frame's CPU profile and stage times into the capture */
void end_frame(const float *profMs, const char *const *profNames, int nProf, const uint32_t *counts,
               const char *const *countNames, int nCounts, double execMs, double subMs, double d3dMs);
/* (present) the pending capture wants the frame's targets as pictures:
 * the directory to write them to, else null */
const char *dump_dir();

/* ---- the submit stage (the submit thread, a worker, the executor itself) -- */
/* A draw about to be queued: its record (id ~0u when not capturing) */
uint32_t draw_begin(const Draw &d);
/* A merged draw's final part and slot counts (merged_queue) */
void draw_parts(uint32_t id, uint16_t parts, uint8_t slots);
/* A merged draw's area: the union of its parts' boxes (merged_queue; the
 * record held its first part's, which misled: a five-part road draw of 2.2
 * Mpx in a 0.1 Mpx box - the road edge-on, its triangles over each other) */
void draw_area(uint32_t id, long x0, long y0, long x1, long y1);
/* A binding the cache issued (D3DB_* bit): counted into the next record */
void bind(uint32_t d3db);
uint32_t take_binds();
uint32_t take_cpu_ns();
extern thread_local int32_t t_run;          /* the run whose draws the thread submits (-1 none) */
extern thread_local uint16_t t_part;        /* its part at hand */
extern thread_local uint8_t t_mergeWhy;     /* merge_join's last verdict (MergeWhy) */
extern thread_local const char *t_note;     /* the next GS batch record's note (sub_gs: a covering batch drawn, why) */
extern thread_local uint8_t t_thread;       /* 0 the executor, 1 the submit thread, 2.. the workers */

/* ---- the D3D thread (inside the queued calls) --------------------------- */
/* Either side of the draw of record id (~0u: nothing) */
void gpu_begin(uint32_t id);
void gpu_end(uint32_t id);

/* ---- the executor ------------------------------------------------------- */
/* A command executed (type: CmdType; 255 a replayed run as one), its time */
void cmd(uint32_t index, uint8_t type, uint32_t ns, int32_t run);
/* A resolved run replayed */
void run(int32_t seq, uint64_t key, uint32_t nCmds, uint32_t parts, uint32_t states, uint32_t steps, uint32_t ns,
         uint32_t texFree, uint32_t texWalk, uint32_t cuts, uint32_t works);
/* The next run's capture number (the executor; -1 when not capturing) */
int32_t next_run();

}  /* namespace cap */
}  /* namespace pglr */
