/*
 * fz_common.c — generator, mutator, guarded buffer and run control of the fuzz targets
 * (see fz_common.h).
 */
#include "fz_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- Constants -------------------------------------------------------------------------- */

/* splitmix64 constants [S. Vigna, "splitmix64.c", public domain, 2015; the same constants as
 * Steele, Lea, Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014, Sec. 3]. */
#define SPLITMIX_GAMMA   0x9E3779B97F4A7C15ULL
#define SPLITMIX_MUL_A   0xBF58476D1CE4E5B9ULL
#define SPLITMIX_MUL_B   0x94D049BB133111EBULL
#define SPLITMIX_SHIFT_A 30U
#define SPLITMIX_SHIFT_B 27U
#define SPLITMIX_SHIFT_C 31U
/* Mutations applied to one input: 1 to 4 (task brief: bit flips, byte sets, boundary values,
 * truncation, extension, splicing). No technical reason for the upper bound of 4. */
#define MUTATIONS_MAX    4U
#define MUTATION_KINDS   9U
#define BYTE_SET_COUNT   4U
#define EDGE_COUNT       13U
#define BITS_PER_BYTE    8U
#define BYTE_MASK        0xFFU
/* A page is 16 KB on Apple silicon and 4 KB on Intel; the guard page uses the larger. */
#define GUARD_BYTES      16384U
/* An input that takes longer than this is reported as slow (hang suspect): 1 s, about 10^6 times
 * the longest input seen on the first run. A bound chosen for this audit, not from a
 * requirement. */
#define SLOW_NS          1000000000ULL
/* An input that runs for this long is a hang: SIGALRM stops the run and saves it. */
#define HANG_SECONDS     30U
#define SLOWEST_KEPT     10U
#define NS_PER_S         1000000000ULL
#define FAIL_KINDS_MAX   16U
#define FAIL_NAME_BYTES  16U
/* Saved files and log lines per failure kind; the rest are only counted. No technical reason. */
#define FAIL_SAVED_MAX   20U
#define PATH_BYTES       512U
#define LOG_LINE_BYTES   1024U
#define LOAD_SAMPLES     3

/* ---- State ------------------------------------------------------------------------------ */

typedef struct {
    uint64_t index;
    uint64_t ns;
} slow_entry_t;

typedef struct {
    char name[FAIL_NAME_BYTES];
    uint64_t count;
    uint32_t saved;
} fail_kind_t;

static FILE *g_log;
static char g_name[FAIL_NAME_BYTES * 2U];
static char g_dir[PATH_BYTES];
static uint64_t g_seed;
static char g_command[LOG_LINE_BYTES];
static slow_entry_t g_slowest[SLOWEST_KEPT];
static fail_kind_t g_kinds[FAIL_KINDS_MAX];
static uint32_t g_kind_count;
static uint64_t g_failures;
static uint64_t g_start_ns;
static uint64_t g_input_start_ns;
static volatile uint64_t g_cur_index;
static const uint8_t *volatile g_cur_data;
static volatile size_t g_cur_length;
static volatile sig_atomic_t g_in_input;

/* ---- Generator -------------------------------------------------------------------------- */

/**
 * @brief One splitmix64 step.
 *
 * Used both to spread a (seed, index) pair into a state and as the generator itself: it passes
 * the tests of its author and is fast; the fuzzer needs uneven bytes, not cryptographic ones.
 *
 * @param[in,out] state  the 64-bit state, advanced
 * @return 64 random bits
 */
static uint64_t splitmix(uint64_t *state)
{
    uint64_t z;

    *state += SPLITMIX_GAMMA;
    z = *state;
    z = (z ^ (z >> SPLITMIX_SHIFT_A)) * SPLITMIX_MUL_A;
    z = (z ^ (z >> SPLITMIX_SHIFT_B)) * SPLITMIX_MUL_B;
    return z ^ (z >> SPLITMIX_SHIFT_C);
}

/**
 * @brief Starts the generator of one input.
 *
 * The state depends on the seed and the index only, so `--one index` rebuilds the same input.
 *
 * @param[out]    rng    the generator
 * @param[in]     seed   the run's seed
 * @param[in]     index  the input's index
 */
void fz_rng_seed(fz_rng_t *rng, uint64_t seed, uint64_t index)
{
    uint64_t mix = seed;

    rng->state = splitmix(&mix) ^ (index * SPLITMIX_GAMMA);
    (void)splitmix(&rng->state);
}

/**
 * @brief Returns the next 64 random bits.
 *
 * @param[in,out] rng  the generator
 * @return 64 random bits
 */
uint64_t fz_rng_next(fz_rng_t *rng)
{
    return splitmix(&rng->state);
}

/**
 * @brief Returns a number from 0 to bound - 1.
 *
 * A modulo of 64 bits by a 32-bit bound: the bias is below 2^-32, no concern for a fuzzer.
 *
 * @param[in,out] rng    the generator
 * @param[in]     bound  number of values, at least 1
 * @return the number
 */
uint32_t fz_rng_below(fz_rng_t *rng, uint32_t bound)
{
    return (bound == 0U) ? 0U : (uint32_t)(fz_rng_next(rng) % bound);
}

/**
 * @brief Returns true with probability per_mille / 1000.
 *
 * @param[in,out] rng        the generator
 * @param[in]     per_mille  chance in thousandths
 * @return true or false
 */
bool fz_rng_chance(fz_rng_t *rng, uint32_t per_mille)
{
    return fz_rng_below(rng, 1000U) < per_mille;
}

/**
 * @brief Returns a 32-bit boundary value.
 *
 * The list is the usual signed and unsigned limits of 8, 16 and 32 bits; a value is also moved
 * one up or one down, because off-by-one errors sit next to the limit.
 *
 * @param[in,out] rng  the generator
 * @return a boundary value
 */
uint32_t fz_rng_edge32(fz_rng_t *rng)
{
    static const uint32_t edges[EDGE_COUNT] = {
        0U, 1U, 0x7FU, 0x80U, 0xFFU, 0x100U, 0x7FFFU, 0x8000U, 0xFFFFU, 0x10000U, 0x7FFFFFFFU,
        0x80000000U, 0xFFFFFFFFU};
    uint32_t value = edges[fz_rng_below(rng, EDGE_COUNT)];
    uint32_t step = fz_rng_below(rng, 3U);

    if (step == 1U) {
        return value + 1U;
    }
    return (step == 2U) ? (value - 1U) : value;
}

/* ---- Mutator ---------------------------------------------------------------------------- */

/**
 * @brief Writes a little-endian value of 1, 2 or 4 bytes inside a buffer, if it fits.
 *
 * @param[in,out] buf    the buffer
 * @param[in]     len    bytes in use
 * @param[in]     field  offset and width of the field
 * @param[in]     value  value to write (its low bytes)
 */
static void put_field(uint8_t *buf, size_t len, const fz_field_t *field, uint32_t value)
{
    for (uint32_t i = 0U; i < field->width; i++) {
        if ((field->offset + i) < len) {
            buf[field->offset + i] = (uint8_t)(value >> (BITS_PER_BYTE * i));
        }
    }
}

/**
 * @brief Applies one random mutation.
 *
 * @param[in,out] rng          the generator
 * @param[in,out] buf          the input
 * @param[in]     len          bytes in use
 * @param[in]     cap          size of buf
 * @param[in]     fields       fields that boundary values go to
 * @param[in]     field_count  number of fields
 * @param[in]     other        a second seed for splicing
 * @param[in]     other_len    its length
 * @return the new length
 */
static size_t mutate_once(fz_rng_t *rng, uint8_t *buf, size_t len, size_t cap,
                          const fz_field_t *fields, uint32_t field_count, const uint8_t *other,
                          size_t other_len)
{
    static const uint8_t byte_sets[BYTE_SET_COUNT] = {0x00U, 0xFFU, 0x7FU, 0x80U};
    uint32_t kind = fz_rng_below(rng, MUTATION_KINDS);
    size_t at = (len > 0U) ? (size_t)fz_rng_below(rng, (uint32_t)len) : 0U;

    switch (kind) {
    case 0U:
        if (len > 0U) {
            buf[at] ^= (uint8_t)(1U << fz_rng_below(rng, BITS_PER_BYTE));
        }
        break;
    case 1U:
        if (len > 0U) {
            buf[at] = byte_sets[fz_rng_below(rng, BYTE_SET_COUNT)];
        }
        break;
    case 2U:
    case 3U:
        if (field_count > 0U) {
            put_field(buf, len, &fields[fz_rng_below(rng, field_count)], fz_rng_edge32(rng));
        }
        break;
    case 4U:
        len = (len > 0U) ? (size_t)fz_rng_below(rng, (uint32_t)len) : 0U;
        break;
    case 5U: {
        size_t add = 1U + (size_t)fz_rng_below(rng, 64U);
        for (size_t i = 0U; (i < add) && (len < cap); i++) {
            buf[len] = (uint8_t)fz_rng_next(rng);
            len++;
        }
        break;
    }
    case 6U:
        if ((other != NULL) && (other_len > 0U) && (len > 0U)) {
            size_t from = (size_t)fz_rng_below(rng, (uint32_t)other_len);
            size_t count = 1U + (size_t)fz_rng_below(rng, (uint32_t)(other_len - from));
            for (size_t i = 0U; (i < count) && ((at + i) < len); i++) {
                buf[at + i] = other[from + i];
            }
        }
        break;
    case 7U:
        if ((len < cap) && (len > 0U)) {
            memmove(&buf[at + 1U], &buf[at], len - at);
            buf[at] = (uint8_t)fz_rng_next(rng);
            len++;
        }
        break;
    case 8U:
        if (len > 0U) {
            memmove(&buf[at], &buf[at + 1U], len - at - 1U);
            len--;
        }
        break;
    }
    return len;
}

/**
 * @brief Applies 1 to 4 random mutations to an input.
 *
 * @param[in,out] rng          the generator
 * @param[in,out] buf          the input, cap bytes of room
 * @param[in]     len          bytes in use
 * @param[in]     cap          size of buf
 * @param[in]     fields       fields that boundary values go to (may be NULL)
 * @param[in]     field_count  number of fields
 * @param[in]     other        a second seed for splicing (may be NULL)
 * @param[in]     other_len    its length
 * @return the new length, at most cap
 */
size_t fz_mutate(fz_rng_t *rng, uint8_t *buf, size_t len, size_t cap, const fz_field_t *fields,
                 uint32_t field_count, const uint8_t *other, size_t other_len)
{
    uint32_t rounds = 1U + fz_rng_below(rng, MUTATIONS_MAX);

    for (uint32_t i = 0U; i < rounds; i++) {
        len = mutate_once(rng, buf, len, cap, fields, field_count, other, other_len);
    }
    return len;
}

/* ---- Guarded buffer --------------------------------------------------------------------- */

/**
 * @brief Maps a buffer that ends at an inaccessible page.
 *
 * @param[out]    guarded  the buffer
 * @param[in]     size     largest length that will be asked for
 */
void fz_guarded_init(fz_guarded_t *guarded, size_t size)
{
    size_t usable = ((size + GUARD_BYTES - 1U) / GUARD_BYTES) * GUARD_BYTES;
    uint8_t *map = mmap(NULL, usable + GUARD_BYTES, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);

    if (map == MAP_FAILED) {
        perror("fz_guarded_init: mmap");
        exit(2);
    }
    if (mprotect(&map[usable], GUARD_BYTES, PROT_NONE) != 0) {
        perror("fz_guarded_init: mprotect");
        exit(2);
    }
    guarded->base = map;
    guarded->pages_bytes = usable;
    guarded->size = size;
}

/**
 * @brief Returns the address of a buffer of `length` bytes that ends at the guard page.
 *
 * @param[in]     guarded  the mapping
 * @param[in]     length   bytes wanted, at most the size given to fz_guarded_init
 * @return pointer to the first byte
 */
uint8_t *fz_guarded_at(const fz_guarded_t *guarded, size_t length)
{
    return &guarded->base[guarded->pages_bytes - length];
}

/* ---- Run control ------------------------------------------------------------------------ */

/**
 * @brief Returns the monotonic time in nanoseconds.
 *
 * @return nanoseconds since an arbitrary start
 */
static uint64_t now_ns(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * NS_PER_S) + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Writes the input being run to a file, for the crash and hang handler.
 *
 * Not strictly async-signal-safe (snprintf); on a crash the run ends here either way.
 *
 * @param[in]     why  "crash" or "hang"
 */
static void save_current(const char *why)
{
    char path[PATH_BYTES];
    int fd;

    (void)snprintf(path, sizeof path, "%s/%s-%s-seed-%llu-idx-%llu.bin", g_dir, g_name, why,
                   (unsigned long long)g_seed, (unsigned long long)g_cur_index);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if ((fd >= 0) && (g_cur_data != NULL)) {
        (void)write(fd, g_cur_data, g_cur_length);
    }
    if (fd >= 0) {
        (void)close(fd);
    }
    (void)fprintf(stderr, "%s: %s on input %llu (seed %llu); saved %s\n", g_name, why,
                  (unsigned long long)g_cur_index, (unsigned long long)g_seed, path);
    if (g_log != NULL) {
        (void)fprintf(g_log, "FAIL %s: %s on input %llu (seed %llu); saved %s\n", g_name, why,
                      (unsigned long long)g_cur_index, (unsigned long long)g_seed, path);
        (void)fflush(g_log);
    }
}

/**
 * @brief Signal handler for a crash (SIGABRT from UBSan, SIGSEGV, SIGBUS, SIGILL, SIGTRAP) and
 * for the hang timer (SIGALRM).
 *
 * @param[in]     signal_number  the signal
 */
static void on_signal(int signal_number)
{
    save_current((signal_number == SIGALRM) ? "hang" : "crash");
    _exit((signal_number == SIGALRM) ? 4 : 3);
}

/**
 * @brief Parses the command line.
 *
 * @param[out]    options  the options, defaults first
 * @param[in]     argc     argument count
 * @param[in]     argv     arguments
 */
void fz_options_parse(fz_options_t *options, int argc, char **argv)
{
    memset(options, 0, sizeof(*options));
    options->count = 1000000ULL;   /* task brief: at least 10^6 inputs per target */
    options->seed = 1ULL;
    options->log_dir = ".";
    for (int i = 1; i < argc; i++) {
        bool has_value = (i + 1) < argc;
        if ((strcmp(argv[i], "--seed") == 0) && has_value) {
            options->seed = strtoull(argv[++i], NULL, 0);
        } else if ((strcmp(argv[i], "--n") == 0) && has_value) {
            options->count = strtoull(argv[++i], NULL, 0);
        } else if ((strcmp(argv[i], "--log") == 0) && has_value) {
            options->log_dir = argv[++i];
        } else if ((strcmp(argv[i], "--one") == 0) && has_value) {
            options->one = true;
            options->one_index = strtoull(argv[++i], NULL, 0);
        } else if ((strcmp(argv[i], "--dir") == 0) && has_value &&
                   (options->dir_count < (sizeof options->dirs / sizeof options->dirs[0]))) {
            options->dirs[options->dir_count++] = argv[++i];
        } else {
            (void)fprintf(stderr, "usage: %s [--seed S] [--n N] [--log DIR] [--one INDEX] "
                                  "[--dir IMAGEDIR]...\n", argv[0]);
            exit(2);
        }
    }
}

/**
 * @brief Opens the log file and installs the handlers.
 *
 * @param[in]     name     target name, also the log file name
 * @param[in]     options  the run's options
 * @param[in]     argc     argument count, for the command line in the log header
 * @param[in]     argv     arguments
 */
void fz_run_open(const char *name, const fz_options_t *options, int argc, char **argv)
{
    char path[PATH_BYTES];
    const char *header = getenv("FZ_HEADER");
    size_t used = 0U;
    static const int signals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGALRM};

    (void)snprintf(g_name, sizeof g_name, "%s", name);
    (void)snprintf(g_dir, sizeof g_dir, "%s", options->log_dir);
    g_seed = options->seed;
    for (int i = 0; i < argc; i++) {
        used += (size_t)snprintf(&g_command[used], sizeof g_command - used, "%s%s",
                                 (i > 0) ? " " : "",
                                 argv[i]);
        if (used >= sizeof g_command) {
            break;
        }
    }
    (void)mkdir(g_dir, 0755);
    (void)snprintf(path, sizeof path, "%s/%s.log", g_dir, name);
    g_log = fopen(path, options->one ? "a" : "w");
    if (g_log == NULL) {
        perror(path);
        exit(2);
    }
    fz_log("%s", (header != NULL) ? header : "FZ_HEADER not set (run through the Makefile)");
    fz_log("target %s; command: %s", name, g_command);
    fz_log("seed %llu; mutation-based fuzzer without coverage feedback (no libFuzzer runtime on "
           "this Mac); UBSan on, -fno-sanitize-recover=undefined", (unsigned long long)g_seed);
    for (size_t i = 0U; i < (sizeof signals / sizeof signals[0]); i++) {
        (void)signal(signals[i], on_signal);
    }
    {
        double load[LOAD_SAMPLES] = {0.0, 0.0, 0.0};
        (void)getloadavg(load, LOAD_SAMPLES);
        fz_log("load averages at the start (1, 5, 15 min): %.2f %.2f %.2f", load[0], load[1],
               load[2]);
    }
    g_start_ns = now_ns();
}

/**
 * @brief Prints a line to the log file and the standard output.
 *
 * @param[in]     format  printf format
 */
void fz_log(const char *format, ...)
{
    char line[LOG_LINE_BYTES];
    va_list args;

    va_start(args, format);
    (void)vsnprintf(line, sizeof line, format, args);
    va_end(args);
    (void)printf("%s\n", line);
    if (g_log != NULL) {
        (void)fprintf(g_log, "%s\n", line);
        (void)fflush(g_log);
    }
    (void)fflush(stdout);
}

/**
 * @brief Marks the start of one input.
 *
 * @param[in]     index   the input's index
 * @param[in]     data    its bytes (kept for the crash handler; may be NULL)
 * @param[in]     length  their number
 */
void fz_input_begin(uint64_t index, const uint8_t *data, size_t length)
{
    g_cur_index = index;
    g_cur_data = data;
    g_cur_length = length;
    g_in_input = 1;
    (void)alarm(HANG_SECONDS);
    g_input_start_ns = now_ns();
}

/**
 * @brief Marks the end of one input and keeps the ten slowest.
 */
void fz_input_end(void)
{
    uint64_t took = now_ns() - g_input_start_ns;

    g_in_input = 0;
    if (took > g_slowest[SLOWEST_KEPT - 1U].ns) {
        uint32_t at = SLOWEST_KEPT - 1U;
        while ((at > 0U) && (g_slowest[at - 1U].ns < took)) {
            g_slowest[at] = g_slowest[at - 1U];
            at--;
        }
        g_slowest[at].ns = took;
        g_slowest[at].index = g_cur_index;
    }
    if (took > SLOW_NS) {
        fz_fail("slow", g_cur_index, g_cur_data, g_cur_length, "input took more than 1 s");
    }
}

/**
 * @brief Returns the table entry of a failure kind, adding it on first use.
 *
 * @param[in]     kind  short word
 * @return the entry
 */
static fail_kind_t *kind_entry(const char *kind)
{
    for (uint32_t i = 0U; i < g_kind_count; i++) {
        if (strncmp(g_kinds[i].name, kind, FAIL_NAME_BYTES - 1U) == 0) {
            return &g_kinds[i];
        }
    }
    if (g_kind_count < FAIL_KINDS_MAX) {
        (void)snprintf(g_kinds[g_kind_count].name, FAIL_NAME_BYTES, "%s", kind);
        return &g_kinds[g_kind_count++];
    }
    return &g_kinds[FAIL_KINDS_MAX - 1U];
}

/**
 * @brief Records a failure: counts it, logs the first ones, saves the input.
 *
 * @param[in]     kind    short word naming the kind of failure
 * @param[in]     index   index of the input (with the seed, it rebuilds the input)
 * @param[in]     data    the input's bytes (may be NULL)
 * @param[in]     length  their number
 * @param[in]     note    one line of detail
 */
void fz_fail(const char *kind, uint64_t index, const uint8_t *data, size_t length,
             const char *note)
{
    fail_kind_t *entry = kind_entry(kind);
    char path[PATH_BYTES];

    g_failures++;
    entry->count++;
    if (entry->saved >= FAIL_SAVED_MAX) {
        return;
    }
    entry->saved++;
    (void)snprintf(path, sizeof path, "%s/%s-fail-%s-%u-seed-%llu-idx-%llu.bin", g_dir, g_name,
                   kind, entry->saved, (unsigned long long)g_seed, (unsigned long long)index);
    if (data != NULL) {
        FILE *file = fopen(path, "wb");
        if (file != NULL) {
            (void)fwrite(data, 1U, length, file);
            (void)fclose(file);
        }
    }
    fz_log("FAIL %s: input %llu (seed %llu): %s; saved %s; reproduce: %s --one %llu", kind,
           (unsigned long long)index, (unsigned long long)g_seed, note, path, g_command,
           (unsigned long long)index);
}

/**
 * @brief Returns the number of failures recorded.
 *
 * @return failures so far
 */
uint64_t fz_fail_count(void)
{
    return g_failures;
}

/**
 * @brief Prints the summary and returns the exit status.
 *
 * @param[in]     inputs_run  number of inputs run
 * @return 0 if no failure was recorded, 1 otherwise
 */
int fz_run_finish(uint64_t inputs_run)
{
    double seconds = (double)(now_ns() - g_start_ns) / (double)NS_PER_S;
    double load[LOAD_SAMPLES] = {0.0, 0.0, 0.0};

    (void)alarm(0);
    (void)getloadavg(load, LOAD_SAMPLES);
    fz_log("inputs run: %llu in %.1f s (%.0f inputs/s); load averages at the end %.2f %.2f %.2f",
           (unsigned long long)inputs_run, seconds, (seconds > 0.0) ? (double)inputs_run / seconds
               : 0.0,
           load[0], load[1], load[2]);
    fz_log("slowest %u inputs (index: time):", SLOWEST_KEPT);
    for (uint32_t i = 0U; i < SLOWEST_KEPT; i++) {
        fz_log("  %llu: %.3f ms", (unsigned long long)g_slowest[i].index,
               (double)g_slowest[i].ns / 1.0e6);
    }
    fz_log("failures: %llu", (unsigned long long)g_failures);
    for (uint32_t i = 0U; i < g_kind_count; i++) {
        fz_log("  %s: %llu (first %u saved)", g_kinds[i].name,
               (unsigned long long)g_kinds[i].count, g_kinds[i].saved);
    }
    fz_log("result: %s", (g_failures == 0U) ? "no failure" : "FAILURES");
    if (g_log != NULL) {
        (void)fclose(g_log);
        g_log = NULL;
    }
    return (g_failures == 0U) ? 0 : 1;
}
