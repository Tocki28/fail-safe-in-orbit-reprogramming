/*
 * fz_common.h — what the fuzz targets of test/controller/stress/fuzz share: a seeded generator,
 * a byte mutator, a guarded buffer, and the run control (log, slowest inputs, saved failures,
 * a crash and hang handler).
 *
 * The fuzzer is mutation-based and has no coverage feedback: Apple clang 17 on this Mac has no
 * libFuzzer runtime and AFL and honggfuzz are not installed (stress audit brief, Part A.1).
 * Every input is a pure function of (seed, index): `--one <index>` rebuilds exactly one input.
 *
 * Host tests only (CODE_RULES A10b): nothing here reaches a flight binary.
 */
#ifndef FZ_COMMON_H
#define FZ_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Generator -------------------------------------------------------------------------- */

/* State of one generator. */
typedef struct {
    uint64_t state;
} fz_rng_t;

/* Start a generator for input `index` of a run with `seed`. */
void fz_rng_seed(fz_rng_t *rng, uint64_t seed, uint64_t index);
/* Next 64 random bits. */
uint64_t fz_rng_next(fz_rng_t *rng);
/* Uniform number 0 to bound - 1; bound must be at least 1. */
uint32_t fz_rng_below(fz_rng_t *rng, uint32_t bound);
/* True with probability per_mille / 1000. */
bool fz_rng_chance(fz_rng_t *rng, uint32_t per_mille);
/* A 32-bit boundary value: 0, 1, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
 * 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF or one of those plus or minus 1. */
uint32_t fz_rng_edge32(fz_rng_t *rng);

/* ---- Mutator ---------------------------------------------------------------------------- */

/* A field of an input that boundary values are written to. */
typedef struct {
    uint32_t offset;   /* first byte of the field */
    uint32_t width;    /* 1, 2 or 4 bytes, little-endian */
} fz_field_t;

/* Apply 1 to 4 mutations to buf[0..len): bit flip; a byte set to 0x00, 0xFF, 0x7F or 0x80; a
 * boundary value written into one of `fields`; truncation; extension; a range copied from
 * `other`; a byte inserted or deleted. Returns the new length, at most cap. */
size_t fz_mutate(fz_rng_t *rng, uint8_t *buf, size_t len, size_t cap, const fz_field_t *fields,
                 uint32_t field_count, const uint8_t *other, size_t other_len);

/* ---- Guarded buffer --------------------------------------------------------------------- */

/* A buffer of `size` bytes that ends exactly at an inaccessible page: a read past its end is a
 * SIGSEGV or SIGBUS, which the crash handler reports as a crash of that input. */
typedef struct {
    uint8_t *base;     /* start of the mapping */
    size_t pages_bytes;
    size_t size;
} fz_guarded_t;

/* Map a guarded buffer able to hold up to `size` bytes; stops the program if the mapping fails. */
void fz_guarded_init(fz_guarded_t *guarded, size_t size);
/* Pointer to `length` bytes (at most the size given to init) that end at the guard page. */
uint8_t *fz_guarded_at(const fz_guarded_t *guarded, size_t length);

/* ---- Run control ------------------------------------------------------------------------ */

/* Options from the command line: --seed S --n N --log DIR --one INDEX --dir PATH (repeatable). */
typedef struct {
    uint64_t seed;
    uint64_t count;
    const char *log_dir;
    bool one;
    uint64_t one_index;
    const char *dirs[4];
    uint32_t dir_count;
} fz_options_t;

/* Read the command line; stops the program with a usage line on an unknown option. */
void fz_options_parse(fz_options_t *options, int argc, char **argv);
/* Open DIR/name.log, write the header line (environment FZ_HEADER) and install the crash and
 * hang handlers. */
void fz_run_open(const char *name, const fz_options_t *options, int argc, char **argv);
/* Print one line to the log and to the standard output. */
void fz_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* Input `index` is about to run: remember it for the crash handler and start its timer. */
void fz_input_begin(uint64_t index, const uint8_t *data, size_t length);
/* The input ended: update the slowest list and the hang timer. */
void fz_input_end(void);
/* Record a failure of `kind` (a short word): counts it, logs the first ones with the reproduce
 * command, and saves the input as a file under the log directory. */
void fz_fail(const char *kind, uint64_t index, const uint8_t *data, size_t length,
             const char *note);
/* Number of failures recorded so far. */
uint64_t fz_fail_count(void);
/* Print the summary (inputs run, wall time, slowest inputs, failures, load average) and return
 * the exit status: 0 with no failure, 1 otherwise. */
int fz_run_finish(uint64_t inputs_run);

#ifdef __cplusplus
}
#endif

#endif /* FZ_COMMON_H */
