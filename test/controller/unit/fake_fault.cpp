/*
 * fake_fault.cpp — the fault reaction on the host (see fake_fault.hpp).
 */
#include "fake_fault.hpp"

#include <cstdio>
#include <cstdlib>

std::jmp_buf fault_exit;
bool fault_armed = false;
volatile bool fault_hit = false;
volatile uint32_t fault_calls = 0U;
const char *volatile fault_last_where = nullptr;

/**
 * @brief Clears the record of fault reactions.
 */
void faultClear()
{
    fault_calls = 0U;
    fault_last_where = nullptr;
}

/**
 * @brief Host stand-in for the firmware's fault reaction: records where, then leaves by longjmp
 * to fault_exit when a test expects it.
 *
 * @param[in]     where  the function that found the impossible value
 * @note Does not return; stops the test program if no test expected a fault.
 */
void faultImpossibleValue(const char *where)
{
    fault_calls = fault_calls + 1U;
    fault_last_where = where;
    if (fault_armed) {
        fault_armed = false;
        fault_hit = true;
        std::longjmp(fault_exit, 1);
    }
    std::printf("FAIL  fault reaction in %s, and no test expected it\n", where);
    std::exit(1);
}

/**
 * @brief Host stand-in for the C fault reaction of the shared code (common/fault.h): forwards, as
 * the board's main.cpp does.
 *
 * @param[in]     where  the function that found the impossible value
 * @note Does not return.
 */
extern "C" void fault_impossible_value(const char *where)
{
    faultImpossibleValue(where);
}
