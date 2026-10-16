/*
 * fz_image.c — fuzz target 3: the image header and image checks. The firmware's
 * image_check_contents and image_check_signatures [common/image_check.h:38,45] and the
 * bootloader's bl_image_check_contents and bl_image_check_signatures [bootloader/bl_image_check.h:
 * 22,28] are run on the same mutated image and must agree on every input (REQ-119: the two share
 * no code; owner decision 35).
 *
 * Three oracles:
 *   1. the two implementations against each other (result code, signed length);
 *   2. a third reading of the check order of bl_image_check.h (empty, proprietary string, format,
 *      kind, target, length, start address, body check value) written here with zlib's CRC-32
 *      [Interfaces Sec. 4.0; zlib crc32]; it is a restatement of the written order, so a defect
 *      the two share shows;
 *   3. an image that both accept must be a byte-for-byte copy (header, body and both signatures)
 *      of a seed that is meant to pass: a mutated image accepted is a finding (REQ-005, REQ-006),
 *      and so is an accepted copy of a wrong-key or flipped seed.
 * Seeds: the signed images of the build (argv --dir). Mutations: bit flips, byte sets, boundary
 * values written at the header fields and at the reset vector, truncation, extension, splicing
 * two seeds; in 30 percent of the inputs the reset vector and the body check value are repaired
 * after the mutation, so that the later checks (start address, signatures) are reached.
 * The region is a guarded buffer that ends at an inaccessible page, so a read past the region is
 * a crash.
 *
 * Small-region probes: the two functions are also called with a region smaller than the
 * header, in a child process (the callers never do: bootloader/main.c and update_receiver.cpp
 * pass SLOT_BYTES or LAST_RESORT_BYTES); the result is logged as an observation, not counted.
 */
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include "bl_image_check.h"
#include "fz_common.h"
#include "fz_fault.h"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"

/* ---- Constants -------------------------------------------------------------------------- */

#define SEEDS_MAX          32U
#define REGIONS            3U
#define PATH_BYTES         512U
/* The header is 64 bytes, the two signatures 128 [layout.h:73,76]. */
#define HEADER_BYTES       ((uint32_t)IMAGE_HEADER_BYTES)
#define SIGNATURES_BYTES   ((uint32_t)IMAGE_SIGNATURES_BYTES)
#define FRAME_BYTES_REF    (HEADER_BYTES + SIGNATURES_BYTES)
#define CODE_OFFSET        ((uint32_t)IMAGE_CODE_OFFSET)
/* The reset vector is the second word of the vector table [Arm DUI 0553A, Sec. 2.3.4]. */
#define RESET_VECTOR_AT    (CODE_OFFSET + 4U)
/* Smallest body: the 448 bytes between header and vector table plus two vector words
 * [common/image_check.c, MIN_BODY_BYTES]. */
#define MIN_BODY_REF       ((CODE_OFFSET - HEADER_BYTES) + 8U)
#define CHANCE_RANDOM      30U
#define CHANCE_MUTATE      900U
#define CHANCE_REPAIR      300U
#define CHANCE_WRONG_KIND  150U
#define CHANCE_SPLICE      300U
#define ERASED             0xFFU

typedef struct {
    uint8_t bytes[SLOT_BYTES];
    uint32_t size;
    char path[PATH_BYTES];
    bool must_fail;   /* wrong-key, flipped: never to pass */
} seed_t;

static seed_t g_seeds[SEEDS_MAX];
static uint32_t g_seed_count;
static fz_guarded_t g_guard;

static const uint32_t REGION_START[REGIONS] = {SLOT_A_START, SLOT_B_START, LAST_RESORT_START};
static const uint32_t REGION_BYTES[REGIONS] = {SLOT_BYTES, SLOT_BYTES, LAST_RESORT_BYTES};
static const uint8_t REGION_KIND[REGIONS] = {IMAGE_KIND_MAIN, IMAGE_KIND_MAIN,
                                             IMAGE_KIND_LAST_RESORT};

static const fz_field_t FIELDS[] = {
    {IMAGE_OFFSET_MAGIC, 4U}, {IMAGE_OFFSET_FORMAT, 1U}, {IMAGE_OFFSET_KIND, 1U},
    {IMAGE_OFFSET_TARGET, 2U}, {IMAGE_OFFSET_VERSION, 4U}, {IMAGE_OFFSET_LENGTH, 4U},
    {IMAGE_OFFSET_BODY_CHECK, 4U}, {52U, 4U}, {RESET_VECTOR_AT, 4U}, {CODE_OFFSET, 4U},
    {0U, 4U}, {IMAGE_OFFSET_LENGTH, 1U}, {IMAGE_OFFSET_LENGTH + 1U, 1U}};
#define FIELD_COUNT ((uint32_t)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static uint64_t g_ok_both;
static uint64_t g_sig_checked;
static uint64_t g_result_counts[16];
static uint64_t g_seed_accepts;

/**
 * @brief Reads a little-endian 32-bit value.
 *
 * @param[in]     p  four bytes
 * @return the value
 */
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief Writes a little-endian 32-bit value.
 *
 * @param[out]    p  four bytes
 * @param[in]     v  the value
 */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/**
 * @brief Loads every *.img file of a directory as a seed.
 *
 * @param[in]     dir  directory path
 */
static void load_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *entry;

    while ((d != NULL) && ((entry = readdir(d)) != NULL) && (g_seed_count < SEEDS_MAX)) {
        size_t n = strlen(entry->d_name);
        if ((n < 5U) || (strcmp(&entry->d_name[n - 4U], ".img") != 0)) {
            continue;
        }
        seed_t *seed = &g_seeds[g_seed_count];
        (void)snprintf(seed->path, sizeof seed->path, "%s/%s", dir, entry->d_name);
        FILE *file = fopen(seed->path, "rb");
        if (file == NULL) {
            continue;
        }
        seed->size = (uint32_t)fread(seed->bytes, 1U, sizeof seed->bytes, file);
        (void)fclose(file);
        seed->must_fail = (strstr(entry->d_name, "wrong-key") != NULL) ||
                          (strstr(entry->d_name, "flipped") != NULL);
        if (seed->size >= HEADER_BYTES) {
            g_seed_count++;
        }
    }
    if (d != NULL) {
        (void)closedir(d);
    }
}

/**
 * @brief The check order of bl_image_check.h, written again with zlib's CRC-32.
 *
 * @param[in]     image         the region
 * @param[in]     region_start  chip address of the region
 * @param[in]     region_bytes  size of the region
 * @param[in]     kind          the kind the region may run
 * @param[out]    signed_bytes  header plus body, set on IMAGE_OK
 * @return the result
 */
static image_result_t reference_contents(const uint8_t *image, uint32_t region_start,
                                         uint32_t region_bytes, uint8_t kind,
                                             uint32_t *signed_bytes)
{
    if (get_u32(image) == 0xFFFFFFFFU) {
        return IMAGE_EMPTY;
    }
    if (memcmp(image, "FSIR", 4U) != 0) {
        return IMAGE_BAD_MAGIC;
    }
    if (image[IMAGE_OFFSET_FORMAT] != IMAGE_FORMAT_VERSION) {
        return IMAGE_BAD_FORMAT;
    }
    if (image[IMAGE_OFFSET_KIND] != kind) {
        return IMAGE_BAD_KIND;
    }
    if ((uint32_t)(image[IMAGE_OFFSET_TARGET] | (image[IMAGE_OFFSET_TARGET + 1U] << 8)) !=
        IMAGE_TARGET_CONTROLLER) {
        return IMAGE_BAD_TARGET;
    }
    uint64_t body = get_u32(&image[IMAGE_OFFSET_LENGTH]);
    if ((body < MIN_BODY_REF) || ((body + FRAME_BYTES_REF) > region_bytes)) {
        return IMAGE_BAD_LENGTH;
    }
    uint64_t reset = get_u32(&image[RESET_VECTOR_AT]);
    if ((reset < ((uint64_t)region_start + CODE_OFFSET)) ||
        (reset >= ((uint64_t)region_start + HEADER_BYTES + body))) {
        return IMAGE_BAD_START;
    }
    if ((uint32_t)crc32(0U, &image[HEADER_BYTES],
                        (uInt)body) != get_u32(&image[IMAGE_OFFSET_BODY_CHECK])) {
        return IMAGE_BAD_BODY_CHECK;
    }
    *signed_bytes = HEADER_BYTES + (uint32_t)body;
    return IMAGE_OK;
}

/**
 * @brief Repairs the reset vector and the body check value of a mutated image, so the checks
 * after them are reached.
 *
 * @param[in,out] rng           the generator
 * @param[in,out] image         the region
 * @param[in]     region_start  chip address of the region
 * @param[in]     region_bytes  size of the region
 */
static void repair(fz_rng_t *rng, uint8_t *image, uint32_t region_start, uint32_t region_bytes)
{
    uint32_t body = get_u32(&image[IMAGE_OFFSET_LENGTH]);

    if ((body < MIN_BODY_REF) || ((uint64_t)body + FRAME_BYTES_REF > region_bytes)) {
        body = MIN_BODY_REF + fz_rng_below(rng, 2000U);
        put_u32(&image[IMAGE_OFFSET_LENGTH], body);
    }
    if (fz_rng_chance(rng, 700U)) {
        put_u32(&image[RESET_VECTOR_AT], region_start + CODE_OFFSET + 1U + fz_rng_below(rng, 64U));
    }
    put_u32(&image[IMAGE_OFFSET_BODY_CHECK], (uint32_t)crc32(0U, &image[HEADER_BYTES], body));
}

/**
 * @brief Builds input `index` in the guarded region buffer.
 *
 * @param[in,out] rng     the generator
 * @param[out]    region  which of the three regions (0 slot A, 1 slot B, 2 last-resort)
 * @param[out]    kind    the kind passed to the checks
 * @return the region buffer
 */
static uint8_t *build_input(fz_rng_t *rng, uint32_t *region, uint8_t *kind)
{
    const seed_t *a = &g_seeds[fz_rng_below(rng, g_seed_count)];
    const seed_t *b = &g_seeds[fz_rng_below(rng, g_seed_count)];
    *region = fz_rng_below(rng, REGIONS);
    uint32_t bytes = REGION_BYTES[*region];
    uint8_t *image = fz_guarded_at(&g_guard, bytes);
    size_t len = a->size;

    *kind = fz_rng_chance(rng, CHANCE_WRONG_KIND) ? (uint8_t)fz_rng_below(rng, 6U)
        : REGION_KIND[*region];
    memset(image, ERASED, bytes);
    if (fz_rng_chance(rng, CHANCE_RANDOM)) {
        len = fz_rng_below(rng, bytes);
        for (size_t i = 0U; i < len; i++) {
            image[i] = (uint8_t)fz_rng_next(rng);
        }
        return image;
    }
    memcpy(image, a->bytes, (a->size < bytes) ? a->size : bytes);
    if (len > bytes) {
        len = bytes;
    }
    if (fz_rng_chance(rng, CHANCE_MUTATE)) {
        len = fz_mutate(rng, image, len, bytes, FIELDS, FIELD_COUNT,
                        fz_rng_chance(rng, CHANCE_SPLICE) ? b->bytes : NULL, b->size);
        memset(&image[len], ERASED, bytes - len);
        if (fz_rng_chance(rng, CHANCE_REPAIR)) {
            repair(rng, image, REGION_START[*region], bytes);
        }
    }
    return image;
}

/**
 * @brief Returns true if the first n bytes equal a seed that is meant to pass (or, for a
 * must-fail seed, any seed).
 *
 * @param[in]     image      the accepted region
 * @param[in]     n          bytes covered by the signatures
 * @param[out]    must_fail  set to true if the match is a seed that must never pass
 * @return true if some seed matches
 */
static bool matches_seed(const uint8_t *image, uint32_t n, bool *must_fail)
{
    for (uint32_t i = 0U; i < g_seed_count; i++) {
        if ((g_seeds[i].size >= n) && (memcmp(image, g_seeds[i].bytes, n) == 0)) {
            *must_fail = g_seeds[i].must_fail;
            return true;
        }
    }
    return false;
}

/**
 * @brief Runs one input through both implementations and the reference.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  input index
 */
static void run_input(uint64_t seed, uint64_t index)
{
    fz_rng_t rng;
    uint32_t region;
    uint8_t kind;
    uint32_t signed_c = 0U;
    uint32_t signed_b = 0U;
    uint32_t signed_r = 0U;
    char note[200];

    fz_rng_seed(&rng, seed, index);
    uint8_t *image = build_input(&rng, &region, &kind);
    uint32_t bytes = REGION_BYTES[region];
    uint32_t start = REGION_START[region];

    fz_input_begin(index, image, bytes);
    image_result_t c = image_check_contents(image, start, bytes, kind, &signed_c);
    image_result_t b = bl_image_check_contents(image, start, bytes, kind, &signed_b);
    image_result_t r = reference_contents(image, start, bytes, kind, &signed_r);

    g_result_counts[(unsigned)c & 15U]++;
    if ((c != b) || (c != r) || ((c == IMAGE_OK) && ((signed_c != signed_b)
                                                     || (signed_c != signed_r)))) {
        (void)snprintf(note, sizeof note,
                       "contents: common %d, bootloader %d, reference %d (signed %u %u %u)",
                       (int)c, (int)b, (int)r, signed_c, signed_b, signed_r);
        fz_fail("contents-differ", index, image, bytes, note);
    }
    if ((c == IMAGE_OK) && (b == IMAGE_OK)) {
        image_result_t sc = image_check_signatures(image, signed_c);
        image_result_t sb = bl_image_check_signatures(image, signed_b);

        g_sig_checked++;
        if (sc != sb) {
            (void)snprintf(note, sizeof note, "signatures: common %d, bootloader %d", (int)sc,
                           (int)sb);
            fz_fail("signatures-differ", index, image, bytes, note);
        }
        if ((sc == IMAGE_OK) && (sb == IMAGE_OK)) {
            bool must_fail = false;
            g_ok_both++;
            if (!matches_seed(image, signed_c + SIGNATURES_BYTES, &must_fail)) {
                fz_fail("accepted-mutated", index, image, bytes,
                        "both implementations accept an image that is no seed");
            } else if (must_fail) {
                fz_fail("accepted-wrong-key", index, image, bytes,
                        "accepted a copy of a wrong-key or flipped seed");
            } else {
                g_seed_accepts++;
            }
        }
    }
    fz_input_end();
}

/**
 * @brief Calls both contents checks with a region smaller than the header, in a child process.
 *
 * @param[in]     region_bytes  size of the region
 * @param[in]     which         0: common, 1: bootloader
 * @return a short word for the log: "ok", "fault" or "crash"
 */
static const char *probe_small(uint32_t region_bytes, uint32_t which)
{
    pid_t pid = fork();
    int status = 0;

    if (pid == 0) {
        uint32_t signed_bytes = 0U;
        bool faulted = false;
        uint8_t *image = fz_guarded_at(&g_guard, region_bytes);
        const seed_t *probe_seed = &g_seeds[0];
        for (uint32_t i = 0U; i < g_seed_count; i++) {
            if (strstr(g_seeds[i].path, "/slotA-v1.img") != NULL) {
                probe_seed = &g_seeds[i];
            }
        }
        memset(image, 0x11, region_bytes);
        memcpy(image, probe_seed->bytes, (probe_seed->size < region_bytes) ? probe_seed->size
               : region_bytes);
        (void)signal(SIGALRM, SIG_DFL);
        (void)signal(SIGSEGV, SIG_DFL);
        (void)signal(SIGBUS, SIG_DFL);
        FZ_FAULT_GUARD(faulted, (void)((which == 0U)
                                       ? image_check_contents(image, SLOT_A_START, region_bytes,
                                                              IMAGE_KIND_MAIN, &signed_bytes)
                                       : bl_image_check_contents(image, SLOT_A_START, region_bytes,
                                                                 IMAGE_KIND_MAIN, &signed_bytes)));
        _exit(faulted ? 10 : 0);
    }
    (void)waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        return "crash (signal)";
    }
    return (WEXITSTATUS(status) == 10) ? "fault reaction" : "returned";
}

/**
 * @brief Logs what each implementation does for a region smaller than the header or the frame.
 */
static void log_small_regions(void)
{
    static const uint32_t sizes[] = {0U, 1U, 63U, 64U, 100U, 191U, 192U, 200U, 456U};

    fz_log("small-region observations (the callers pass 0xA000 and 0x4000 only; "
           "region bytes: common / bootloader; the region holds the first bytes of slotA-v1.img):");
    for (size_t i = 0U; i < (sizeof sizes / sizeof sizes[0]); i++) {
        fz_log("  region %u: %s / %s", sizes[i], probe_small(sizes[i], 0U),
               probe_small(sizes[i], 1U));
    }
}

/**
 * @brief Runs the image-check fuzz target.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments: see fz_options_parse
 * @return 0 if no input failed, 1 otherwise
 */
int main(int argc, char **argv)
{
    fz_options_t options;
    uint64_t first = 0U;
    uint64_t last;

    fz_options_parse(&options, argc, argv);
    fz_run_open("fz_image", &options, argc, argv);
    for (uint32_t d = 0U; d < options.dir_count; d++) {
        load_dir(options.dirs[d]);
    }
    if (g_seed_count == 0U) {
        fz_log("no image seeds: pass --dir");
        return 2;
    }
    fz_log("image seeds: %u", g_seed_count);
    fz_guarded_init(&g_guard, SLOT_BYTES);
    last = options.count;
    if (options.one) {
        first = options.one_index;
        last = first + 1U;
    }
    for (uint64_t index = first; index < last; index++) {
        run_input(options.seed, index);
    }
    fz_log("contents results (IMAGE_OK=0 EMPTY=1 MAGIC=2 FORMAT=3 KIND=4 TARGET=5 LENGTH=6 START=7 "
           "BODY=8): %llu %llu %llu %llu %llu %llu %llu %llu %llu",
           (unsigned long long)g_result_counts[0], (unsigned long long)g_result_counts[1],
           (unsigned long long)g_result_counts[2], (unsigned long long)g_result_counts[3],
           (unsigned long long)g_result_counts[4], (unsigned long long)g_result_counts[5],
           (unsigned long long)g_result_counts[6], (unsigned long long)g_result_counts[7],
           (unsigned long long)g_result_counts[8]);
    fz_log("inputs that passed the contents checks and reached the signatures: %llu; accepted by "
           "both: %llu (all unchanged copies of a seed: %llu)", (unsigned long long)g_sig_checked,
           (unsigned long long)g_ok_both, (unsigned long long)g_seed_accepts);
    if (!options.one) {
        log_small_regions();
    }
    return fz_run_finish(last - first);
}
