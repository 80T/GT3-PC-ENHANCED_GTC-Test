/* pgl_models.cpp - decoded VifModel meshes for the VU1 model draws
 * (docs/models.md 9 and 10, docs/renderer.md "Models").
 *
 * model_draw / vif_model_draw hand over a VifModel and a part index; the
 * whole model is decoded once with gt3vif::decodeModel into vertex / index
 * arrays (immutable native buffers) and cached by the model's address.  The
 * first use in each frame checks a hash of the model's header and part
 * table; the part data is hashed in turn (each model every kVerifyFrames
 * frames, the models spread over them) and, once found changed under the
 * same table (logged), at each first use in a frame.  The game says when it
 * writes a model (pglr_model_loaded: vif_model_relocate; pglr_model_written:
 * TStripBuilder::finish - the tyre and meter packets, rebuilt in place), which decodes it
 * again at its next use.  Parts drawn with positions sent by vu1_unpack_xyz
 * (preXyz) are decoded for the draw into the dynamic stream.
 *
 * PC: a model the game loads (pglr_model_loaded: vif_model_relocate) is
 * decoded ahead on the worker (pgl_prewarm.cpp) from a copy taken then; its
 * first draw takes that mesh when the model still hashes as the copy did. */
#include "engine/pgl/native/pgl_d3d.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <windows.h>

#include "platform/log.h"

namespace pglr {

namespace {

/* A model's part data is hashed every this many frames (its slot among
 * them). */
constexpr uint32_t kVerifyFrames = 32;

struct MeshEntry {
    gt3vif::Mesh mesh;
    std::vector<uint32_t> partFirst;    /* part p's batches: [partFirst[p], partFirst[p + 1]) (empty: unsorted) */
    Com<ID3D11Buffer> vb, ib;
    uint64_t tableHash = 0;         /* the header and the part table */
    uint64_t dataHash = 0;          /* the parts' data */
    uint64_t checkedFrame = 0;
    uint64_t lastFrame = 0;
    double lastUse = 0.0;           /* g_execSeconds when last drawn */
    size_t bytes = 0;               /* its decoded vertices and indices */
    uint32_t verifySlot = 0;        /* the frame (mod kVerifyFrames) its data is hashed */
    bool ok = false;
    bool written = false;           /* the game wrote it (pglr_model_written / _loaded) */
    bool changing = false;          /* its data changed under the same table: hashed each frame */
};

std::unordered_map<const void *, std::unique_ptr<MeshEntry>> g_meshes;
/* PC: the decoded meshes are kept while they fit in kMeshBudget bytes (the
 * least recently drawn go first beyond it, never one drawn in the last
 * kMeshKeep seconds) or until unused for kMeshExpire seconds: a course's
 * models stay decoded from lap to lap (at 600 frames unused they went in
 * under three seconds at a high frame rate, decoded again every lap - the
 * stutters where a stretch of the course came back into view). */
constexpr size_t kMeshBudget = 256u << 20;
constexpr double kMeshKeep = 5.0, kMeshExpire = 600.0;
size_t g_meshBytes;
gt3vif::Mesh g_tmp;
uint64_t g_frame = 1;
uint32_t g_nextSlot;
/* The last model looked up (a model's parts are drawn one after another);
 * forgotten as a frame ends (the entries erased then). */
const void *g_lastModel;
MeshEntry *g_lastEntry;
/* The models the game wrote (pglr_model_written, any thread), applied at
 * the next lookup. */
std::mutex g_writtenLock;
std::vector<const void *> g_written;
std::atomic<bool> g_writtenAny{false};

/* A fast 64-bit hash (four independent lanes over 32-byte steps: memory
 * bound, several GB/s). */
uint64_t hash_words(const void *p, size_t bytes, uint64_t h)
{
    const uint8_t *s = static_cast<const uint8_t *>(p);
    if (bytes >= 32) {
        uint64_t a = h ^ 0x9E3779B97F4A7C15ull, b = h + 0xC2B2AE3D27D4EB4Full, c = h ^ 0x165667B19E3779F9ull,
                 d = h - 0x27D4EB2F165667C5ull;
        while (bytes >= 32) {
            uint64_t w[4];
            memcpy(w, s, 32);
            a = (a ^ w[0]) * 0xFF51AFD7ED558CCDull;
            b = (b ^ w[1]) * 0xC4CEB9FE1A85EC53ull;
            c = (c ^ w[2]) * 0xFF51AFD7ED558CCDull;
            d = (d ^ w[3]) * 0xC4CEB9FE1A85EC53ull;
            a ^= a >> 29;
            b ^= b >> 31;
            c ^= c >> 29;
            d ^= d >> 31;
            s += 32;
            bytes -= 32;
        }
        h = a ^ (b * 3) ^ (c * 5) ^ (d * 7);
    }
    while (bytes >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 31;
        s += 8;
        bytes -= 8;
    }
    while (bytes-- > 0)
        h = (h ^ *s++) * 0x100000001B3ull;
    return h;
}

/* The header and the part table (the data's addresses and sizes). */
uint64_t table_hash(const void *model)
{
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    return hash_words(model, sizeof(*hdr) + (size_t)hdr->nParts * sizeof(gt3vif::VifPartRecord), 0x51ED270B27u);
}

/* The parts' data (the VIF packets). */
uint64_t data_hash(const void *model)
{
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    uint64_t h = 0x2545F4914F6CDD1Dull;
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        if (parts[i].data != 0)
            h = hash_words(reinterpret_cast<const void *>((uintptr_t)parts[i].data), (size_t)parts[i].qwc * 16, h);
    }
    return h;
}

/* The models written since the last lookup: decoded again at their next
 * use. */
void take_written()
{
    std::vector<const void *> w;
    {
        std::lock_guard<std::mutex> lock(g_writtenLock);
        w.swap(g_written);
        g_writtenAny.store(false, std::memory_order_relaxed);
    }
    for (const void *model : w) {
        auto it = g_meshes.find(model);
        if (it != g_meshes.end()) {
            it->second->written = true;
            it->second->checkedFrame = 0;
        }
    }
}

bool make_buffer(const void *data, size_t bytes, UINT bind, Com<ID3D11Buffer> &out)
{
    out.reset();
    if (bytes == 0)
        return false;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = (UINT)bytes;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = data;
    return SUCCEEDED(dev()->CreateBuffer(&d, &init, out.put()));
}

void log_warnings(const gt3vif::Mesh &m, const void *model)
{
    for (const std::string &w : m.warnings) {
        char key[96];
        snprintf(key, sizeof(key), "pgl-model-warn-%p-%s", model, w.c_str());
        log_once(key, "pgl: model %p: %s", model, w.c_str());
    }
}

/* PC: each batch's vertex box, morph targets included (Batch::bmin / bmax:
 * the screen area a draw writes, vu1_draw). */
void batch_bounds(gt3vif::Mesh &m)
{
    for (gt3vif::Batch &b : m.batches) {
        b.hasBounds = false;
        if (b.vertexCount == 0 || (size_t)b.firstVertex + b.vertexCount > m.vertices.size())
            continue;
        for (int k = 0; k < 3; k++) {
            b.bmin[k] = 1e30f;
            b.bmax[k] = -1e30f;
        }
        for (uint32_t i = 0; i < b.vertexCount; i++) {
            const gt3vif::Vertex &v = m.vertices[b.firstVertex + i];
            for (int k = 0; k < 3; k++) {
                b.bmin[k] = std::min(b.bmin[k], std::min(v.pos[k], v.pos2[k]));
                b.bmax[k] = std::max(b.bmax[k], std::max(v.pos[k], v.pos2[k]));
            }
        }
        b.hasBounds = true;
    }
}

/* PC (CPU time): where each part's batches lie (decodeModel decodes the
 * parts in order), so a part's draw does not scan the whole model's. */
void part_index(const gt3vif::Mesh &m, uint32_t nParts, std::vector<uint32_t> &first)
{
    first.assign((size_t)nParts + 1, 0);
    uint32_t b = 0;
    for (uint32_t p = 0; p <= nParts; p++) {
        while (b < m.batches.size() && m.batches[b].part < p)
            b++;
        first[p] = b;
    }
    for (size_t i = 1; i < m.batches.size(); i++)
        if (m.batches[i].part < m.batches[i - 1].part) {
            first.clear();          /* (not in part order: the whole list) */
            return;
        }
}

/* ---- decoded ahead (pgl_prewarm.cpp) ------------------------------------- */

/* The meshes the worker decoded of the models the game loaded, by address,
 * until a draw takes them (take_pre); a model loaded there again replaces
 * its own, those no draw took in kMeshExpire seconds go (models_end_frame).
 * g_preSkip: the models a draw decoded itself before the worker was done
 * (their table hash): the worker drops those.  (Never freed, the lock has no
 * destructor: the worker may still run while the process exits.) */
struct PreMesh {
    gt3vif::Mesh mesh;
    std::vector<uint32_t> partFirst;
    Com<ID3D11Buffer> vb, ib;
    uint64_t tableHash = 0, dataHash = 0;
    size_t bytes = 0;
    double made = 0.0;              /* pre::seconds() */
    bool ok = false;
};
SRWLOCK g_preLock = SRWLOCK_INIT;
std::unordered_map<const void *, std::unique_ptr<PreMesh>> *g_pre =
    new std::unordered_map<const void *, std::unique_ptr<PreMesh>>();
std::unordered_map<const void *, uint64_t> *g_preSkip = new std::unordered_map<const void *, uint64_t>();
std::atomic<uint32_t> g_preCount{0};

/* A model as the game loaded it: its header and part table (part i's data
 * word now i + 1, 0 without data) and its parts' data, copied on the game
 * thread (models_loaded); the table hashed there. */
struct MeshJob : pre::Job {
    const void *model = nullptr;
    uint64_t tableHash = 0;
    std::vector<uint8_t> table;
    std::vector<uint8_t> data;
    std::vector<size_t> at;         /* part i's data in `data' */
    void run() override;
};

const void *resolve_copy(uint32_t word, void *user)
{
    const MeshJob *j = static_cast<const MeshJob *>(user);
    if (word == 0 || word > j->at.size())
        return nullptr;
    return j->data.data() + j->at[word - 1];
}

/* (the worker) */
void MeshJob::run()
{
    std::unique_ptr<PreMesh> p(new PreMesh());
    const gt3vif::VifModelHeader *hdr = reinterpret_cast<const gt3vif::VifModelHeader *>(table.data());
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    /* data_hash as the game's memory gives it (the same bytes, part by
     * part) */
    uint64_t h = 0x2545F4914F6CDD1Dull;
    for (uint32_t i = 0; i < hdr->nParts; i++)
        if (parts[i].data != 0)
            h = hash_words(data.data() + at[i], (size_t)parts[i].qwc * 16, h);
    p->tableHash = tableHash;
    p->dataHash = h;
    gt3vif::DecodeOptions opt;
    p->ok = gt3vif::decodeModel(table.data(), resolve_copy, this, opt, p->mesh) && !p->mesh.vertices.empty() &&
            !p->mesh.indices.empty();
    log_warnings(p->mesh, model);
    if (p->ok) {
        batch_bounds(p->mesh);
        part_index(p->mesh, hdr->nParts, p->partFirst);
        p->ok = make_buffer(p->mesh.vertices.data(), p->mesh.vertices.size() * sizeof(gt3vif::Vertex),
                            D3D11_BIND_VERTEX_BUFFER, p->vb) &&
                make_buffer(p->mesh.indices.data(), p->mesh.indices.size() * 4, D3D11_BIND_INDEX_BUFFER, p->ib);
    }
    p->bytes = p->mesh.vertices.size() * sizeof(gt3vif::Vertex) + p->mesh.indices.size() * 4;
    p->made = pre::seconds();
    pre::made(pre::MODEL, 1, p->bytes);
    std::unique_ptr<PreMesh> old;           /* (released after the lock) */
    AcquireSRWLockExclusive(&g_preLock);
    auto s = g_preSkip->find(model);
    if (s != g_preSkip->end() && s->second == tableHash) {
        g_preSkip->erase(s);
        old = std::move(p);
    } else {
        std::unique_ptr<PreMesh> &slot = (*g_pre)[model];
        old = std::move(slot);
        slot = std::move(p);
        g_preCount.store((uint32_t)g_pre->size(), std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_preLock);
}

/* The mesh decoded ahead for model into e, if the model is still what was
 * copied (its table hash th; its data's dh, hashed here unless `hashed').
 * Else, with the worker on, the model is noted as decoded at the draw. */
bool take_pre(const void *model, uint64_t th, bool hashed, uint64_t dh, MeshEntry *e)
{
    if (!pre::on())
        return false;
    std::unique_ptr<PreMesh> p;
    AcquireSRWLockExclusive(&g_preLock);
    if (g_preCount.load(std::memory_order_relaxed) != 0) {
        auto it = g_pre->find(model);
        if (it != g_pre->end()) {
            p = std::move(it->second);
            g_pre->erase(it);
            g_preCount.store((uint32_t)g_pre->size(), std::memory_order_relaxed);
        }
    }
    if (!p) {
        if (g_preSkip->size() > 4096)
            g_preSkip->clear();
        (*g_preSkip)[model] = th;
    }
    ReleaseSRWLockExclusive(&g_preLock);
    if (!p || p->tableHash != th)
        return false;
    if (!hashed)
        dh = data_hash(model);
    if (p->dataHash != dh)
        return false;
    e->mesh = std::move(p->mesh);
    e->partFirst = std::move(p->partFirst);
    e->vb = std::move(p->vb);
    e->ib = std::move(p->ib);
    e->ok = p->ok;
    e->tableHash = th;
    e->dataHash = dh;
    e->written = false;
    g_meshBytes -= e->bytes;
    e->bytes = p->bytes;
    g_meshBytes += e->bytes;
    pre::taken(pre::MODEL);
    return true;
}

}  /* namespace */

bool model_mesh(const PglrModelPart &part, const gt3vif::Mesh **mesh, ID3D11Buffer **vb, ID3D11Buffer **ib,
                uint32_t *baseVertex, uint32_t *baseIndex, uint32_t *firstBatch, uint32_t *endBatch)
{
    if (part.model == nullptr)
        return false;
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(part.model);
    if (part.part < 0 || part.part >= (int)hdr->nParts)
        return false;

    if (part.preXyz != nullptr) {
        /* positions sent in front of the part: decode it for this draw */
        const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
        const gt3vif::VifPartRecord &pr = parts[part.part];
        if (pr.data == 0)
            return false;
        ProfScope prof(PROF_MESH_DECODE);   /* (the verbose monitor's profile) */
        gt3vif::DecodeOptions opt;
        opt.preXyz = part.preXyz;
        opt.preXyzCount = part.preXyzCount;
        g_tmp.clear();
        g_tmp.mode = hdr->mode;
        g_tmp.scaleFlags = hdr->scaleFlags;
        if (!gt3vif::decodePart(reinterpret_cast<const void *>((uintptr_t)pr.data), pr.qwc, pr.tex,
                                (uint16_t)part.part, hdr->mode, opt, g_tmp) ||
            g_tmp.vertices.empty() || g_tmp.indices.empty())
            return false;
        batch_bounds(g_tmp);
        *baseVertex = stream_vertices(g_tmp.vertices.data(), sizeof(gt3vif::Vertex),
                                      (uint32_t)g_tmp.vertices.size(), vb);
        *baseIndex = stream_indices(g_tmp.indices.data(), (uint32_t)g_tmp.indices.size(), ib);
        *mesh = &g_tmp;
        *firstBatch = 0;
        *endBatch = (uint32_t)g_tmp.batches.size();
        return true;
    }

    if (g_writtenAny.load(std::memory_order_relaxed))
        take_written();
    MeshEntry *e = g_lastEntry;
    if (part.model != g_lastModel) {
        std::unique_ptr<MeshEntry> &slot = g_meshes[part.model];
        if (!slot) {
            slot.reset(new MeshEntry());
            slot->verifySlot = g_nextSlot++ % kVerifyFrames;
        }
        e = slot.get();
        g_lastModel = part.model;
        g_lastEntry = e;
    }
    if (e->checkedFrame != g_frame) {
        const uint64_t th = table_hash(part.model);
        bool stale = !e->ok || e->written || th != e->tableHash;
        uint64_t dh = 0;
        bool hashed = false;
        if (!stale && (e->changing || (g_frame + e->verifySlot) % kVerifyFrames == 0)) {
            dh = data_hash(part.model);
            hashed = true;
            if (dh != e->dataHash) {
                stale = true;
                if (!e->changing) {
                    e->changing = true;
                    char key[48];
                    snprintf(key, sizeof(key), "pgl-model-changed-%p", part.model);
                    log_once(key, "pgl: model %p changed in place (its data, not its part table): checked every frame",
                             part.model);
                }
            }
        }
        const bool fresh = e->tableHash == 0;   /* (never decoded: the log's counts) */
        if (stale && take_pre(part.model, th, hashed, dh, e))
            stale = false;                  /* PC: decoded ahead (pgl_prewarm.cpp) */
        else if (stale && fresh && pre::on())
            pre::late(pre::MODEL);
        if (stale) {
            ProfScope prof(PROF_MESH_DECODE);
            gt3vif::DecodeOptions opt;
            e->mesh.clear();
            e->ok = gt3vif::decodeModel(part.model, e->mesh, opt) && !e->mesh.vertices.empty() &&
                    !e->mesh.indices.empty();
            log_warnings(e->mesh, part.model);
            if (e->ok) {
                batch_bounds(e->mesh);
                part_index(e->mesh, hdr->nParts, e->partFirst);
            }
            if (e->ok) {
                e->ok = make_buffer(e->mesh.vertices.data(), e->mesh.vertices.size() * sizeof(gt3vif::Vertex),
                                    D3D11_BIND_VERTEX_BUFFER, e->vb) &&
                        make_buffer(e->mesh.indices.data(), e->mesh.indices.size() * 4, D3D11_BIND_INDEX_BUFFER,
                                    e->ib);
            }
            e->tableHash = th;
            e->dataHash = hashed ? dh : data_hash(part.model);
            e->written = false;
            g_meshBytes -= e->bytes;
            e->bytes = e->mesh.vertices.size() * sizeof(gt3vif::Vertex) + e->mesh.indices.size() * 4;
            g_meshBytes += e->bytes;
        }
        e->checkedFrame = g_frame;
        e->lastUse = g_execSeconds;
    }
    e->lastFrame = g_frame;
    if (!e->ok)
        return false;
    *mesh = &e->mesh;
    *vb = e->vb.get();
    *ib = e->ib.get();
    *baseVertex = 0;
    *baseIndex = 0;
    if ((size_t)part.part + 1 < e->partFirst.size()) {
        *firstBatch = e->partFirst[part.part];
        *endBatch = e->partFirst[part.part + 1];
    } else {
        *firstBatch = 0;
        *endBatch = (uint32_t)e->mesh.batches.size();
    }
    return true;
}

void models_written(const void *model)
{
    if (model == nullptr)
        return;
    std::lock_guard<std::mutex> lock(g_writtenLock);
    g_written.push_back(model);
    g_writtenAny.store(true, std::memory_order_relaxed);
}

void models_loaded(const void *model)
{
    models_written(model);
    if (model == nullptr || !pre::on())
        return;
    const gt3vif::VifModelHeader *hdr = static_cast<const gt3vif::VifModelHeader *>(model);
    const gt3vif::VifPartRecord *parts = reinterpret_cast<const gt3vif::VifPartRecord *>(hdr + 1);
    MeshJob *j = new MeshJob();
    j->model = model;
    j->tableHash = table_hash(model);
    const uint8_t *tbl = static_cast<const uint8_t *>(model);
    j->table.assign(tbl, tbl + sizeof(*hdr) + (size_t)hdr->nParts * sizeof(gt3vif::VifPartRecord));
    j->at.resize(hdr->nParts);
    size_t total = 0;
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        j->at[i] = total;
        if (parts[i].data != 0)
            total += (size_t)parts[i].qwc * 16;
    }
    j->data.resize(total);
    gt3vif::VifPartRecord *copy = reinterpret_cast<gt3vif::VifPartRecord *>(j->table.data() + sizeof(*hdr));
    for (uint32_t i = 0; i < hdr->nParts; i++) {
        if (parts[i].data != 0 && parts[i].qwc != 0)
            memcpy(j->data.data() + j->at[i], reinterpret_cast<const void *>((uintptr_t)parts[i].data),
                   (size_t)parts[i].qwc * 16);
        copy[i].data = parts[i].data != 0 ? i + 1 : 0;
    }
    /* (a draw of an earlier model here decoded itself: not this one) */
    AcquireSRWLockExclusive(&g_preLock);
    g_preSkip->erase(model);
    ReleaseSRWLockExclusive(&g_preLock);
    pre::post(j);
}

void models_end_frame()
{
    g_lastModel = nullptr;
    g_lastEntry = nullptr;
    g_frame++;
    if ((g_frame % 64) != 0)
        return;
    /* those unused for kMeshExpire seconds; then, over the budget, the least
     * recently drawn (not within kMeshKeep seconds) */
    const double now = g_execSeconds;
    /* PC: the meshes decoded ahead that no draw took in as long (released
     * after the lock) */
    if (g_preCount.load(std::memory_order_relaxed) != 0) {
        std::vector<std::unique_ptr<PreMesh>> gone;
        AcquireSRWLockExclusive(&g_preLock);
        for (auto it = g_pre->begin(); it != g_pre->end();) {
            if (it->second->made + kMeshExpire < now) {
                gone.push_back(std::move(it->second));
                it = g_pre->erase(it);
            } else {
                ++it;
            }
        }
        g_preCount.store((uint32_t)g_pre->size(), std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_preLock);
    }
    std::vector<std::pair<double, const void *>> old;
    for (auto it = g_meshes.begin(); it != g_meshes.end();) {
        if (it->second->lastUse + kMeshExpire < now) {
            g_meshBytes -= it->second->bytes;
            it = g_meshes.erase(it);
            continue;
        }
        if (it->second->lastUse + kMeshKeep < now)
            old.push_back({it->second->lastUse, it->first});
        ++it;
    }
    if (g_meshBytes <= kMeshBudget)
        return;
    std::sort(old.begin(), old.end());
    for (const auto &o : old) {
        if (g_meshBytes <= kMeshBudget)
            break;
        auto it = g_meshes.find(o.second);
        if (it == g_meshes.end())
            continue;
        g_meshBytes -= it->second->bytes;
        g_meshes.erase(it);
    }
}

}  /* namespace pglr */
