# ocudu_janus_e3 — gNB-side E3 publish helpers.
#
# Kept as its own small target for one reason: the convert loop REQUIRES AVX2 and
# F16C, and those flags are scoped to this library rather than added globally.
#
# Why not rely on ocudu's -march=native (CMakeLists.txt:641)?
#
#   1. MARCH is a cache variable. Building a portable image with -DMARCH=x86-64
#      is a normal thing to do, and it would silently drop the vector path.
#   2. `native` means the BUILDER's CPU, not the target's. If the image is built
#      on one machine and run on another, native is both unsafe (may emit
#      instructions the target lacks) and unreliable (may omit AVX2 if the
#      builder is older).
#   3. Raising the whole RAN's ISA baseline is a portability decision far beyond
#      this feature. AVX2 and F16C are Haswell-era (2013), so requiring them for
#      one translation unit is mild; requiring them everywhere is not.
#
# The stakes: the scalar fallback measures 2,490 us for a 4-port slot against a
# 500 us slot period — 47x slower — and this code runs inline in the PHY RX
# thread. e3_slot_convert.h therefore #errors when no vector path is compiled in,
# and these flags are what make that guard a guarantee instead of a hope.

# ---------------------------------------------------------------------------
# Shared-contract drift check
# ---------------------------------------------------------------------------
# jbpf_e3_ids.h, jbpf_e3_slot_api.h and e3_slot_convert.h are shared with
# E3Controller (codelets/include/) — the codelets, the offline verifier, the
# controller's ShmIqWriter and this helper must all agree on them.
#
# They are COPIED here because the two repos have separate build systems. That
# is the same hazard the codelet side already hit: three byte-identical copies of
# jbpf_srsran_contexts.h floating around with nothing enforcing agreement. The
# stakes are higher here — a divergent scale or row geometry produces rows that
# differ with NO error, because bf16 and fp16 are both 2 bytes.
#
# So: when an E3Controller checkout is reachable, diff at configure time and fail
# on drift. Pass -DE3CONTROLLER_DIR=/path/to/E3Controller, or accept the default
# sibling layout.
set(E3CONTROLLER_DIR "${CMAKE_SOURCE_DIR}/../E3Controller"
    CACHE PATH "E3Controller checkout, for the shared E3 contract headers")
set(_e3_shared_src "${E3CONTROLLER_DIR}/codelets/include")

if (EXISTS "${_e3_shared_src}/jbpf_e3_slot_api.h")
    foreach (_hdr jbpf_e3_ids.h jbpf_e3_slot_api.h e3_slot_convert.h jbpf_e3_prb_block_api.h)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E compare_files
                    "${_e3_shared_src}/${_hdr}" "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}"
            RESULT_VARIABLE _e3_diff OUTPUT_QUIET ERROR_QUIET)
        if (NOT _e3_diff EQUAL 0)
            message(FATAL_ERROR
                "E3 contract drift: ocudu_janus/e3/${_hdr} differs from\n"
                "  ${_e3_shared_src}/${_hdr}\n"
                "Resync it and rebuild BOTH the codelets and this helper — a changed "
                "layout or scale makes the two writers disagree with no runtime error.")
        endif ()
    endforeach ()
    message(STATUS "ocudu_janus_e3: E3 contract headers match ${_e3_shared_src}")
else ()
    message(STATUS
        "ocudu_janus_e3: no E3Controller checkout at ${E3CONTROLLER_DIR}; "
        "using the vendored contract headers unchecked")
endif ()

add_library(ocudu_janus_e3 STATIC jbpf_e3_publish.cpp)

target_include_directories(ocudu_janus_e3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link jbpf for jbpf_register_helper_function() — and, just as importantly, for
# the build ORDER it implies.
#
# jbpf builds its vendored concurrency-kit as an external project that installs
# headers into ${CMAKE_BINARY_DIR}/libck-jbpf/include at BUILD time, and
# jbpf.h -> ck_epoch.h -> ck_stack.h needs them. Without this dependency the
# directory does not exist when this target compiles, the include falls through
# to a system-installed /usr/include/ck_stack.h, and that older header is not
# C++-clean:
#
#   /usr/include/ck_stack.h:279: invalid conversion from 'void*' to
#   'ck_stack_entry*' [-fpermissive]
#
# ocudu_upper_phy never hit this only because it already links jbpf.
target_link_libraries(ocudu_janus_e3 PUBLIC jbpf)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(ocudu_janus_e3 PRIVATE -mavx2 -mf16c)
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    # e3_slot_convert.h takes the NEON path on aarch64; nothing extra needed.
else ()
    message(FATAL_ERROR
        "ocudu_janus_e3: no vector convert path for CMAKE_SYSTEM_PROCESSOR="
        "'${CMAKE_SYSTEM_PROCESSOR}'. The scalar fallback is ~47x slower and this "
        "code runs in the PHY RX thread. Port e3_slot_convert.h, or define "
        "E3_ALLOW_SCALAR_CONVERT if a scalar convert there is genuinely acceptable.")
endif ()

# Unit tests for the helper bodies (attach validation, row addressing, ring
# advance, byte-exact output). Standalone: stubs jbpf_register_helper_function
# rather than linking jbpf core.
if (BUILD_TESTING)
    add_executable(test_jbpf_e3_publish test_jbpf_e3_publish.cpp)
    target_include_directories(test_jbpf_e3_publish PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(test_jbpf_e3_publish PRIVATE
        $<TARGET_PROPERTY:jbpf,INTERFACE_INCLUDE_DIRECTORIES>)
    add_dependencies(test_jbpf_e3_publish jbpf)

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(test_jbpf_e3_publish PRIVATE -mavx2 -mf16c)
    endif ()
    target_link_libraries(test_jbpf_e3_publish PRIVATE rt)
    add_test(jbpf_e3_publish_test test_jbpf_e3_publish)
endif (BUILD_TESTING)
