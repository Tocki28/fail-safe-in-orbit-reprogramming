/*
 * fz_fault.cpp — the fault reaction on the host for the C++ fuzz targets (see fz_fault.h): the
 * firmware's faultImpossibleValue (firmware/fault.hpp) and the C entry the shared code calls
 * (common/fault.h).
 */
#include "fz_fault.h"

#include <cstdio>
#include <cstdlib>

#include "fault.hpp"

jmp_buf fz_fault_exit;
volatile bool fz_fault_armed = false;
volatile uint32_t fz_fault_calls = 0U;
volatile uint8_t fz_fault_location = 0U;
const char *volatile fz_fault_where = nullptr;

/**
 * @brief Clears the record of fault reactions.
 */
extern "C" void fz_fault_clear(void)
{
    fz_fault_calls = 0U;
    fz_fault_location = 0U;
    fz_fault_where = nullptr;
}

/**
 * @brief Host stand-in for the firmware's fault reaction: records the call, then leaves the
 * guarded code by longjmp.
 *
 * Outside a guarded call it prints the place and aborts, so the crash handler saves the input
 * that reached it.
 *
 * @param[in]     location  the fault-location code (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
void faultImpossibleValue(uint8_t location, const char *where)
{
    fz_fault_calls = fz_fault_calls + 1U;
    fz_fault_location = location;
    fz_fault_where = where;
    if (fz_fault_armed) {
        fz_fault_armed = false;
        longjmp(fz_fault_exit, 1);
    }
    std::fprintf(stderr, "fault reaction outside a guarded call, in %s\n", where);
    std::abort();
}

/**
 * @brief Host stand-in for the C fault reaction of the shared code (common/fault.h): forwards, as
 * the board's main.cpp does.
 *
 * @param[in]     location  the fault-location code (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
extern "C" void fault_impossible_value(uint8_t location, const char *where)
{
    faultImpossibleValue(location, where);
}
