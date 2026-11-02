/*
 * test_jbpf_e3_publish — unit tests for the E3 publish helper (B2).
 *
 * Includes the .cpp directly so the tests can reach the helper bodies, which
 * live in an anonymous namespace on purpose (they are reached from codelets via
 * jbpf's relocation table, not by name).
 *
 * Covers the two things that are easy to get silently wrong:
 *   - row addressing / ring advance, where an arithmetic slip corrupts a
 *     neighbouring row instead of failing, and
 *   - the attach-time validation, which is the only thing standing between a
 *     geometry disagreement and rows written at the wrong stride.
 *
 * Build:
 *   g++ -std=c++17 -O2 -mavx2 -mf16c -I. -I<jbpf>/src/core -I<jbpf>/src/common \
 *       -I<jbpf>/3p/ck/include test_jbpf_e3_publish.cpp -o test_e3_publish -lrt
 */

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "jbpf_e3_publish.cpp"

/*
 * Registration needs a live jbpf agent, and these tests exercise the helper
 * BODIES rather than the plumbing that installs them. Stub it so the test links
 * standalone instead of dragging in jbpf core.
 */
extern "C" int
jbpf_register_helper_function(jbpf_helper_func_def_t)
{
    return 0;
}

using namespace ocudu::janus;

namespace {

int g_failures = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  FAIL: %s  (%s:%d)\n", (what), __FILE__, __LINE__); \
            ++g_failures;                                                     \
        } else {                                                              \
            std::printf("  ok:   %s\n", (what));                              \
        }                                                                     \
    } while (0)

constexpr std::uint16_t kSym  = 14;
constexpr std::uint16_t kSubc = 3276;
constexpr std::uint32_t kAnts = 4;
constexpr std::uint32_t kRows = 3; /* small, so wrap-around is quick to hit */

const char* kShmName = "/e3_test_ran_buffers";

std::size_t
u16_per_ant()
{
    return static_cast<std::size_t>(kSym) * kSubc * 2u;
}

/* Build a region laid out exactly as E3Controller's ShmIqWriter::open() does. */
void*
make_region(std::size_t& out_total, std::uint32_t rows = kRows, std::uint32_t ants = kAnts)
{
    const std::uint32_t num_fh_samples = ants * static_cast<std::uint32_t>(u16_per_ant());
    const std::uint32_t row_bytes      = num_fh_samples * 2u;
    const std::size_t   total          = sizeof(SharedMemoryHeader) + std::size_t(kNumBuffers) * rows * row_bytes;

    ::shm_unlink(kShmName);

#define MUST_OK(expr, what)                                                   \
    do {                                                                      \
        if (!(expr)) {                                                        \
            std::perror(what);                                                \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

    int fd = ::shm_open(kShmName, O_CREAT | O_RDWR, 0666);
    MUST_OK(fd >= 0, "shm_open");
    MUST_OK(::ftruncate(fd, static_cast<off_t>(total)) == 0, "ftruncate");
    void* m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    MUST_OK(m != MAP_FAILED, "mmap");
#undef MUST_OK
    std::memset(m, 0, total);

    auto* h                     = static_cast<SharedMemoryHeader*>(m);
    h->version                  = 1;
    h->fh_buffer_size           = rows * row_bytes;
    h->num_fh_samples           = num_fh_samples;
    h->num_fh_rows              = rows;
    out_total                   = total;
    return m;
}

e3_shm_cfg
make_cfg(std::uint16_t epoch = 1, float scale = 1.0f)
{
    e3_shm_cfg c{};
    c.version         = E3_SHM_CFG_VERSION;
    c.epoch           = epoch;
    c.nof_symbols     = kSym;
    c.nof_subcarriers = kSubc;
    c.scale           = scale;
    std::snprintf(c.name, sizeof(c.name), "%s", kShmName);
    return c;
}

std::uint64_t
attach(const e3_shm_cfg& c)
{
    return h_e3_attach(reinterpret_cast<std::uint64_t>(&c), sizeof(c), 0, 0, 0);
}

std::uint64_t
publish(const void* src, std::size_t len, const e3_slot_sel& sel)
{
    return h_e3_publish_slot(reinterpret_cast<std::uint64_t>(src), len, reinterpret_cast<std::uint64_t>(&sel),
                             sizeof(sel), 0);
}

e3_slot_sel
make_sel()
{
    e3_slot_sel s{};
    s.version             = E3_SLOT_SEL_VERSION;
    s.nof_slots_per_frame = 20;
    s.slot_mask           = 0; /* publish everything */
    return s;
}

} // namespace

int
main()
{
    std::printf("test_jbpf_e3_publish\n");
    std::printf("vector path: %s\n\n", e3_publish_vector_path_name());

    std::size_t total = 0;
    void*       region = make_region(total);

    /* ---- attach validation ---- */
    std::printf("attach validation:\n");
    {
        auto c = make_cfg();
        c.version = 999;
        CHECK(attach(c) == std::uint64_t(-1), "rejects wrong cfg version");
    }
    {
        auto c = make_cfg();
        c.scale = 0.0f;
        CHECK(attach(c) == std::uint64_t(-1), "rejects zero scale (would zero the row silently)");
    }
    {
        auto c = make_cfg();
        c.scale = -1.0f;
        CHECK(attach(c) == std::uint64_t(-1), "rejects negative scale");
    }
    {
        auto c            = make_cfg();
        c.nof_subcarriers = 1000; /* header's num_fh_samples no longer divides */
        CHECK(attach(c) == std::uint64_t(-1), "rejects geometry disagreeing with header");
    }
    {
        auto c = make_cfg();
        std::snprintf(c.name, sizeof(c.name), "/e3_definitely_absent");
        CHECK(attach(c) == std::uint64_t(-1), "refuses to create a region it does not own");
    }
    {
        auto c = make_cfg();
        CHECK(attach(c) == 0, "attaches to a well-formed region");
        CHECK(g_region.geom.nof_ants == kAnts, "derives nof_ants from the header");
        CHECK(g_region.rows_per_buf == kRows, "reads rows-per-buffer from the header");
        CHECK(attach(c) == 0, "re-attach with same epoch is idempotent");
    }

    /* ---- publish validation ---- */
    std::printf("\npublish validation:\n");
    const std::size_t          ant_bytes = u16_per_ant() * 2;
    const std::size_t          slot_bytes = ant_bytes * kAnts;
    std::vector<std::uint16_t> src(slot_bytes / 2);
    for (std::size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<std::uint16_t>(0x3f80u ^ (i & 0xffu)); /* ~1.0 in bf16, varied */
    }
    auto sel = make_sel();

    {
        e3_set_src_window(nullptr, 0);
        CHECK(publish(src.data(), slot_bytes, sel) == std::uint64_t(-1), "rejects publish with no source window");
    }

    e3_set_src_window(src.data(), static_cast<std::uint32_t>(slot_bytes));

    {
        std::vector<std::uint8_t> outside(1024, 0);
        CHECK(publish(outside.data(), 1024, sel) == std::uint64_t(-1), "rejects source outside the window");
    }
    {
        auto bad = sel;
        bad.version = 42;
        CHECK(publish(src.data(), slot_bytes, bad) == std::uint64_t(-1), "rejects wrong selector version");
    }
    {
        CHECK(publish(src.data(), ant_bytes - 1, sel) == std::uint64_t(-1),
              "rejects length that is not a whole number of antennas");
    }
    {
        /* Larger than the row: must REFUSE, not write a prefix. */
        e3_set_src_window(src.data(), static_cast<std::uint32_t>(slot_bytes));
        CHECK(publish(src.data(), slot_bytes + ant_bytes, sel) == std::uint64_t(-1),
              "refuses a slot larger than the row rather than truncating");
    }

    /* ---- addressing + ring advance ---- */
    std::printf("\naddressing and ring advance:\n");
    {
        auto* base      = static_cast<std::uint8_t*>(region) + sizeof(SharedMemoryHeader);
        const auto rb   = g_region.row_bytes;
        const auto bb   = g_region.buf_bytes;
        bool       all_ok = true;

        /* Walk two full buffers' worth so we cross the wrap twice. */
        for (std::uint32_t i = 0; i < kNumBuffers * kRows; ++i) {
            std::uint64_t rc = publish(src.data(), slot_bytes, sel);
            if (rc == std::uint64_t(-1)) {
                all_ok = false;
                break;
            }
            std::uint32_t got_buf = static_cast<std::uint32_t>(rc >> 32);
            std::uint32_t got_row = static_cast<std::uint32_t>(rc & 0xffffffffu);
            std::uint32_t exp_buf = i / kRows;
            std::uint32_t exp_row = i % kRows;
            if (got_buf != exp_buf || got_row != exp_row) {
                std::printf("    iter %u: got (buf=%u,row=%u) expected (buf=%u,row=%u)\n", i, got_buf, got_row,
                            exp_buf, exp_row);
                all_ok = false;
            }
            /* The row we were told about must be the one that changed. */
            const std::uint8_t* row = base + std::size_t(got_buf) * bb + std::size_t(got_row) * rb;
            bool nonzero = false;
            for (std::uint32_t k = 0; k < rb; ++k) {
                if (row[k] != 0) {
                    nonzero = true;
                    break;
                }
            }
            if (!nonzero) {
                std::printf("    iter %u: reported row was not written\n", i);
                all_ok = false;
            }
        }
        CHECK(all_ok, "cursor walks rows then wraps to the next buffer, writing the row it reports");

        std::uint64_t rc = publish(src.data(), slot_bytes, sel);
        CHECK(static_cast<std::uint32_t>(rc >> 32) == 0 && static_cast<std::uint32_t>(rc & 0xffffffffu) == 0,
              "wraps back to buffer 0 row 0 after the last buffer");
    }

    /* ---- byte-for-byte against a reference convert ---- */
    std::printf("\noutput bytes:\n");
    {
        std::vector<std::uint16_t> want(slot_bytes / 2, 0);
        e3_convert::RowGeometry    geom{kSym, kSubc, kAnts};
        e3_convert::convert_slot(want.data(), src.data(), geom, kAnts, 1.0f);

        std::uint64_t rc  = publish(src.data(), slot_bytes, sel);
        auto*         base = static_cast<std::uint8_t*>(region) + sizeof(SharedMemoryHeader);
        const std::uint8_t* row = base + std::size_t(rc >> 32) * g_region.buf_bytes +
                                  std::size_t(rc & 0xffffffffu) * g_region.row_bytes;
        CHECK(std::memcmp(row, want.data(), slot_bytes) == 0, "published row matches the shared convert exactly");
    }

    /* ---- fewer ports than the row holds ---- */
    std::printf("\npartial antenna set:\n");
    {
        const std::size_t two_ports = ant_bytes * 2;
        e3_set_src_window(src.data(), static_cast<std::uint32_t>(two_ports));
        std::uint64_t rc = publish(src.data(), two_ports, sel);
        CHECK(rc != std::uint64_t(-1), "accepts a 2-port slot into a 4-port row");

        auto*               base = static_cast<std::uint8_t*>(region) + sizeof(SharedMemoryHeader);
        const std::uint8_t* row  = base + std::size_t(rc >> 32) * g_region.buf_bytes +
                                  std::size_t(rc & 0xffffffffu) * g_region.row_bytes;
        bool tail_zero = true;
        for (std::size_t k = two_ports; k < g_region.row_bytes; ++k) {
            if (row[k] != 0) {
                tail_zero = false;
                break;
            }
        }
        CHECK(tail_zero, "antennas beyond the delivered ports are left untouched (read as silence)");
    }

    ::munmap(region, total);
    ::shm_unlink(kShmName);

    std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
