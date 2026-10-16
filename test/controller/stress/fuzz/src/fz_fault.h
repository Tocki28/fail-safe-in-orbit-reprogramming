/*
 * fz_fault.h — the fault reaction of the flight code on the host, for the fuzz targets: it
 * records the call (count, location code, function name) and leaves the code under test by
 * longjmp, so a target can tell that an input reached it and carry on with the next input.
 *
 * Why a copy of test/controller/unit/fake_fault.cpp and not that file: fake_fault.cpp stops the
 * program when no test expected a fault; a fuzz run must count and go on. The original is read
 * only (stress audit rules) and is not edited.
 *
 * Host tests only (CODE_RULES A10b).
 */
#ifndef FZ_FAULT_H
#define FZ_FAULT_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern jmp_buf fz_fault_exit;
extern volatile bool fz_fault_armed;     /* a guarded call is running: leave by longjmp */
extern volatile uint32_t fz_fault_calls; /* calls since fz_fault_clear() */
extern volatile uint8_t fz_fault_location;
extern const char *volatile fz_fault_where;

/* Clears the count, the location and the name. */
void fz_fault_clear(void);

#ifdef __cplusplus
}
#endif

/* Runs `statement` with the fault reaction armed; sets `faulted` to true if it ended there.
 * setjmp(...) == 0 is the whole controlling expression of the if (ISO/IEC 9899:2011, 7.13.1.1).
 * Host tests only: JPL Power of Ten bans setjmp in flight code (CODE_RULES A9); this is the one
 * way to leave a function declared noreturn. A variable the statement changes must be volatile
 * if it is read after a longjmp. */
#define FZ_FAULT_GUARD(faulted, statement) \
    do {                                   \
        fz_fault_armed = true;             \
        (faulted) = false;                 \
        if (setjmp(fz_fault_exit) == 0) {  \
            statement;                     \
        } else {                           \
            (faulted) = true;              \
        }                                  \
        fz_fault_armed = false;            \
    } while (0)

#endif /* FZ_FAULT_H */
