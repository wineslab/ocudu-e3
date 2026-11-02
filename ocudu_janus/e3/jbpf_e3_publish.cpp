/*
 * jbpf_e3_publish.cpp — gNB-side E3 publish helpers (workstream B2).
 *
 * MUST be compiled with -mavx2 -mf16c. e3_slot_convert.h #errors otherwise; see
 * the note on the scalar fallback in the header.
 */

#include "jbpf_e3_publish.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "jbpf.h"
}

#include "e3_slot_convert.h"
#include "jbpf_e3_ids.h"
#include "jbpf_e3_slot_api.h"

namespace ocudu {
namespace janus {
namespace {

/* ------------------------------------------------------------------ *
 *  Region layout — MUST match E3Controller's ShmIqWriter::open()
 * ------------------------------------------------------------------ */

/*
 * Mirrors SharedMemoryHeader in E3Controller/src/e3sm/utils/e3sm_shm_writer.h,
 * which in turn mirrors
 * spear-aerial-sample-apps/dapps/common/e3_manager/e3_manager.h:39.
 * 64 bytes, asserted below — this is the NVIDIA L1 KPM SM / cuBB contract and
 * the dApp reads it directly.
 */
struct SharedMemoryHeader {
    std::uint32_t version;
    std::uint32_t fh_buffer_size;
    std::uint32_t pusch_buffer_size;
    std::uint32_t hest_buffer_size;
    std::uint32_t num_fh_samples;
    std::uint32_t num_fh_rows;
    std::uint32_t num_pusch_rows;
    std::uint32_t num_hest_rows;
    std::uint32_t max_hest_samples_per_row;
    std::uint32_t reserved[7];
};
static_assert(sizeof(SharedMemoryHeader) == 64, "SHM header layout drift");

/* The controller creates the region with two buffers (ShmIqWriter::open sets
 * num_buffers_ = 2) and advertises only num_fh_rows, so the buffer count is a
 * shared constant rather than a header field. */
constexpr std::uint32_t kNumBuffers = 2;

/* ------------------------------------------------------------------ *
 *  Attached state
 * ------------------------------------------------------------------ */

struct AttachedRegion {
    void*         mapped     = nullptr;
    std::size_t   mapped_len = 0;
    std::uint8_t* rows_base  = nullptr; /* just past the header */

    std::uint32_t row_bytes      = 0;
    std::uint32_t rows_per_buf   = 0;
    std::uint32_t buf_bytes      = 0;
    std::uint16_t epoch          = 0;
    float         scale          = 1.0f;

    e3_convert::RowGeometry geom{0, 0, 0};

    /* Ring cursor. Ownership moved here from ShmIqWriter along with the write
     * itself; the L1 SM was the region's only writer (e3sm_layer_1.cpp:173), so
     * there is no second cursor to race with. Single-threaded: the hook fires
     * from one RU UL execution context per sector. */
    std::uint32_t next_row = 0;
    std::uint8_t  next_buf = 0;

    bool valid = false;
};

AttachedRegion g_region;

/* Source window published by the hook caller. Thread-local: each RU UL context
 * has its own. */
thread_local const std::uint8_t* tls_src_lo = nullptr;
thread_local const std::uint8_t* tls_src_hi = nullptr;

void
detach_locked()
{
    if (g_region.mapped) {
        ::munmap(g_region.mapped, g_region.mapped_len);
    }
    g_region = AttachedRegion{};
}

/* ------------------------------------------------------------------ *
 *  Helper: attach
 * ------------------------------------------------------------------ */

std::uint64_t
h_e3_attach(std::uint64_t cfg_, std::uint64_t cfg_len, std::uint64_t, std::uint64_t, std::uint64_t)
{
    const auto* cfg = reinterpret_cast<const e3_shm_cfg*>(static_cast<std::uintptr_t>(cfg_));

    if (!cfg || cfg_len < sizeof(e3_shm_cfg)) {
        return static_cast<std::uint64_t>(-1);
    }
    if (cfg->version != E3_SHM_CFG_VERSION) {
        std::fprintf(stderr, "[e3_publish] attach: cfg version %u, expected %u\n", cfg->version, E3_SHM_CFG_VERSION);
        return static_cast<std::uint64_t>(-1);
    }
    if (!(cfg->scale > 0.0f)) {
        /* A zero or negative scale zeroes the row and silently breaks the dApp,
         * so refuse rather than "helpfully" defaulting to 1.0 — that would make
         * the gNB disagree with the controller's calibration. */
        std::fprintf(stderr, "[e3_publish] attach: refusing non-positive scale %g\n", static_cast<double>(cfg->scale));
        return static_cast<std::uint64_t>(-1);
    }
    if (cfg->nof_symbols == 0 || cfg->nof_subcarriers == 0) {
        return static_cast<std::uint64_t>(-1);
    }

    /* Already attached to this epoch: idempotent, so the codelet can forward
     * every control-input message without special-casing. */
    if (g_region.valid && g_region.epoch == cfg->epoch) {
        return 0;
    }

    detach_locked();

    char name[sizeof(cfg->name) + 1];
    std::memcpy(name, cfg->name, sizeof(cfg->name));
    name[sizeof(cfg->name)] = '\0';
    if (name[0] == '\0') {
        return static_cast<std::uint64_t>(-1);
    }

    /* O_RDWR without O_CREAT: the E3Controller OWNS this region. If it is not
     * there yet, fail and let the codelet skip the slot — do not race the owner
     * into creating a region with the wrong geometry. */
    int fd = ::shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        std::fprintf(stderr, "[e3_publish] attach: shm_open(%s) failed: %s\n", name, std::strerror(errno));
        return static_cast<std::uint64_t>(-1);
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) <= sizeof(SharedMemoryHeader)) {
        ::close(fd);
        return static_cast<std::uint64_t>(-1);
    }
    const std::size_t total = static_cast<std::size_t>(st.st_size);

    void* m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        std::fprintf(stderr, "[e3_publish] attach: mmap(%zu) failed: %s\n", total, std::strerror(errno));
        return static_cast<std::uint64_t>(-1);
    }

    const auto* hdr = static_cast<const SharedMemoryHeader*>(m);

    /* Cross-check the controller's advertised row against the geometry it also
     * sent us. num_fh_samples is the WHOLE row's uint16 count (all antennas),
     * which is also how the dApp reads it (subcarrier_power_app.cpp:1925). */
    const std::size_t u16_per_ant =
        static_cast<std::size_t>(cfg->nof_symbols) * cfg->nof_subcarriers * 2u;
    if (u16_per_ant == 0 || hdr->num_fh_samples == 0 || (hdr->num_fh_samples % u16_per_ant) != 0) {
        std::fprintf(stderr,
                     "[e3_publish] attach: header num_fh_samples=%u is not a multiple of "
                     "symbols*subc*2=%zu — geometry disagreement, refusing\n",
                     hdr->num_fh_samples, u16_per_ant);
        ::munmap(m, total);
        return static_cast<std::uint64_t>(-1);
    }

    const std::uint32_t nof_ants  = hdr->num_fh_samples / static_cast<std::uint32_t>(u16_per_ant);
    const std::uint32_t row_bytes = hdr->num_fh_samples * static_cast<std::uint32_t>(sizeof(std::uint16_t));

    if (hdr->num_fh_rows == 0) {
        ::munmap(m, total);
        return static_cast<std::uint64_t>(-1);
    }

    /* Confirm the advertised ring actually fits the mapping, so a stale or
     * truncated region cannot make us write past the end. */
    const std::size_t need =
        sizeof(SharedMemoryHeader) + static_cast<std::size_t>(kNumBuffers) * hdr->num_fh_rows * row_bytes;
    if (need > total) {
        std::fprintf(stderr, "[e3_publish] attach: region %zu B too small for %u buffers x %u rows x %u B\n", total,
                     kNumBuffers, hdr->num_fh_rows, row_bytes);
        ::munmap(m, total);
        return static_cast<std::uint64_t>(-1);
    }

    g_region.mapped       = m;
    g_region.mapped_len   = total;
    g_region.rows_base    = static_cast<std::uint8_t*>(m) + sizeof(SharedMemoryHeader);
    g_region.row_bytes    = row_bytes;
    g_region.rows_per_buf = hdr->num_fh_rows;
    g_region.buf_bytes    = hdr->num_fh_rows * row_bytes;
    g_region.epoch        = cfg->epoch;
    g_region.scale        = cfg->scale;
    g_region.geom         = e3_convert::RowGeometry{cfg->nof_symbols, cfg->nof_subcarriers, nof_ants};
    g_region.next_row     = 0;
    g_region.next_buf     = 0;
    g_region.valid        = true;

    std::printf("[e3_publish] attached %s: %u buffers x %u rows x %u B "
                "(%u ants x %u sym x %u subc), scale=%g, epoch=%u, vector=%s\n",
                name, kNumBuffers, g_region.rows_per_buf, g_region.row_bytes, nof_ants, cfg->nof_symbols,
                cfg->nof_subcarriers, static_cast<double>(cfg->scale), cfg->epoch, e3_convert::vector_path_name());
    return 0;
}

/* ------------------------------------------------------------------ *
 *  Helper: publish
 * ------------------------------------------------------------------ */

std::uint64_t
h_e3_publish_slot(std::uint64_t src_, std::uint64_t src_len, std::uint64_t sel_, std::uint64_t sel_len,
                  std::uint64_t)
{
    if (!g_region.valid) {
        return static_cast<std::uint64_t>(-1);
    }

    const auto* src = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(src_));
    const auto* sel = reinterpret_cast<const e3_slot_sel*>(static_cast<std::uintptr_t>(sel_));

    if (!src || src_len == 0 || !sel || sel_len < sizeof(e3_slot_sel)) {
        return static_cast<std::uint64_t>(-1);
    }
    if (sel->version != E3_SLOT_SEL_VERSION) {
        return static_cast<std::uint64_t>(-1);
    }

    /* Runtime source check against the window the hook published. Independent of
     * whether the codelet was verified — and the gNB never verifies. */
    if (tls_src_lo == nullptr || src < tls_src_lo || src + src_len > tls_src_hi) {
        return static_cast<std::uint64_t>(-1);
    }

    /* Refuse, never truncate. Publishing a prefix (e.g. 4 of 8 antennas) with no
     * signal is a silently wrong measurement, which is worse than a gap; the
     * codelet turns this into an explicit TRUNCATED descriptor. */
    if (src_len > g_region.row_bytes) {
        return static_cast<std::uint64_t>(-1);
    }

    const std::size_t u16_per_ant = g_region.geom.u16_per_ant();
    const std::size_t bytes_per_ant = u16_per_ant * sizeof(std::uint16_t);
    if (bytes_per_ant == 0 || (src_len % bytes_per_ant) != 0) {
        return static_cast<std::uint64_t>(-1);
    }
    const std::uint32_t ports = static_cast<std::uint32_t>(src_len / bytes_per_ant);
    if (ports == 0 || ports > g_region.geom.nof_ants) {
        return static_cast<std::uint64_t>(-1);
    }

    /* Destination derived from geometry we own; the codelet supplied no pointer,
     * so it cannot be forged. Same addressing as ShmIqWriter::publish_row_cbf16. */
    const std::uint8_t  buf_idx = g_region.next_buf;
    const std::uint32_t row_idx = g_region.next_row;

    std::uint8_t* row = g_region.rows_base + static_cast<std::size_t>(buf_idx) * g_region.buf_bytes +
                        static_cast<std::size_t>(row_idx) * g_region.row_bytes;

    const std::size_t written =
        e3_convert::convert_slot(reinterpret_cast<std::uint16_t*>(row),
                                 reinterpret_cast<const std::uint16_t*>(src), g_region.geom, ports, g_region.scale);

    /*
     * Clear any antenna slots this slot did not fill.
     *
     * ShmIqWriter's comment says antennas beyond nof_ports "stay zero (the row
     * was zero-filled once in open())" — but that only holds the FIRST time a
     * row is used. Rows are recycled through the ring, so a slot carrying fewer
     * ports than the row holds would otherwise leave the tail containing IQ from
     * an OLDER slot, and the dApp reads a fixed antenna count regardless. The
     * result would be stale samples presented as current ones: silently wrong
     * data, which is exactly the failure class this design exists to remove.
     *
     * Free in the common case — at 4 ports into a 4-antenna row there is no
     * tail. Caught by test_jbpf_e3_publish's "partial antenna set" case, which
     * only fails once rows start being reused.
     *
     * NOTE: the same latent bug exists in E3Controller's publish_row_cbf16.
     */
    if (written < g_region.row_bytes) {
        std::memset(row + written, 0, g_region.row_bytes - written);
    }

    /* Advance the ring exactly as ShmIqWriter did. Overwrite-oldest: a slow
     * reader loses slots, it never backpressures the PHY. */
    if (++g_region.next_row >= g_region.rows_per_buf) {
        g_region.next_row = 0;
        g_region.next_buf = static_cast<std::uint8_t>((g_region.next_buf + 1) % kNumBuffers);
    }

    return (static_cast<std::uint64_t>(buf_idx) << 32) | static_cast<std::uint64_t>(row_idx);
}

} // namespace

/* ------------------------------------------------------------------ *
 *  Public API
 * ------------------------------------------------------------------ */

void
e3_set_src_window(const void* base, std::uint32_t len)
{
    tls_src_lo = static_cast<const std::uint8_t*>(base);
    tls_src_hi = tls_src_lo + len;
}

bool
e3_publish_has_vector_path()
{
    return e3_convert::has_vector_path();
}

const char*
e3_publish_vector_path_name()
{
    return e3_convert::vector_path_name();
}

int
e3_register_publish_helpers()
{
    /* IDs come from the shared jbpf_e3_ids.h, which the codelets and the offline
     * verifier also include. Skew there is what produces a codelet that verifies
     * cleanly and then fails to load with "call to nonexistent function". */
    jbpf_helper_func_def_t attach{};
    std::snprintf(attach.name, sizeof(attach.name), "e3_attach");
    attach.reloc_id    = JBPF_E3_ATTACH_ID;
    attach.function_cb = reinterpret_cast<jbpf_helper_func_t>(&h_e3_attach);

    jbpf_helper_func_def_t publish{};
    std::snprintf(publish.name, sizeof(publish.name), "e3_publish_slot");
    publish.reloc_id    = JBPF_E3_PUBLISH_SLOT_ID;
    publish.function_cb = reinterpret_cast<jbpf_helper_func_t>(&h_e3_publish_slot);

    /* jbpf_register_helper_function returns 0 on insert, 1 on replace, -1 on a
     * bad/out-of-range reloc_id. Treat replace as success (idempotent init). */
    int r1 = jbpf_register_helper_function(attach);
    int r2 = jbpf_register_helper_function(publish);
    if (r1 < 0 || r2 < 0) {
        std::fprintf(stderr, "[e3_publish] helper registration failed (attach=%d publish=%d)\n", r1, r2);
        return -1;
    }

    std::printf("[e3_publish] registered e3_attach=%d e3_publish_slot=%d, convert path=%s\n", JBPF_E3_ATTACH_ID,
                JBPF_E3_PUBLISH_SLOT_ID, e3_convert::vector_path_name());
    return 0;
}

} // namespace janus
} // namespace ocudu
