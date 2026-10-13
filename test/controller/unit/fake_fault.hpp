/*
 * fake_fault.hpp — the fault reaction (src/controller/firmware/fault.hpp, common/fault.h) on the
 * host: fake_fault.cpp records the call and leaves the code under test by longjmp, so a test can
 * show that an impossible value reached the fault reaction and nothing else happened.
 *
 * Host tests only. JPL Power of Ten bans setjmp in flight code (CODE_RULES A9); here it is the one
 * way to leave a function the code under test declares [[noreturn]].
 */
#ifndef FAKE_FAULT_HPP
#define FAKE_FAULT_HPP

#include <csetjmp>
#include <cstdint>

#include "fault.hpp"

extern std::jmp_buf fault_exit;
extern bool fault_armed;               /* a test expects a fault: leave by longjmp */
extern volatile bool fault_hit;        /* the last FAULT_CATCH ended in the fault reaction */
extern volatile uint32_t fault_calls;  /* calls since faultClear() */
extern const char *volatile fault_last_where;

/* Clears fault_calls and fault_last_where. */
void faultClear();

/* Runs `statement` with the fault reaction armed and sets `faulted` to true if it ended there.
 * setjmp(...) == 0 is the whole controlling expression of the if, one of the places the C and
 * C++ standards allow it. A variable the statement changes must not be read after a longjmp
 * unless it is volatile. */
#define FAULT_CATCH(faulted, statement)    \
    do {                                   \
        fault_armed = true;                \
        fault_hit = false;                 \
        if (setjmp(fault_exit) == 0) {     \
            statement;                     \
            fault_armed = false;           \
        }                                  \
        (faulted) = fault_hit;             \
    } while (0)

#endif /* FAKE_FAULT_HPP */
