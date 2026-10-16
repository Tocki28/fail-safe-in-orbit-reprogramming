/*
 * fz_fault_c.c — the fault reaction on the host for the C fuzz targets (see fz_fault.h): the
 * C entry the shared code and the bootloader call (common/fault.h).
 */
#include <stdio.h>
#include <stdlib.h>

#include "fault.h"
#include "fz_fault.h"

jmp_buf fz_fault_exit;
volatile bool fz_fault_armed = false;
volatile uint32_t fz_fault_calls = 0U;
volatile uint8_t fz_fault_location = 0U;
const char *volatile fz_fault_where = NULL;

/**
 * @brief Clears the record of fault reactions.
 */
void fz_fault_clear(void)
{
    fz_fault_calls = 0U;
    fz_fault_location = 0U;
    fz_fault_where = NULL;
}

/**
 * @brief Host stand-in for the fault reaction (common/fault.h): records the call, then leaves
 * the guarded code by longjmp; outside a guarded call it aborts.
 *
 * @param[in]     location  the fault-location code (common/fault_codes.h)
 * @param[in]     where     the function that found the impossible value
 * @note Does not return.
 */
_Noreturn void fault_impossible_value(uint8_t location, const char *where)
{
    fz_fault_calls = fz_fault_calls + 1U;
    fz_fault_location = location;
    fz_fault_where = where;
    if (fz_fault_armed) {
        fz_fault_armed = false;
        longjmp(fz_fault_exit, 1);
    }
    (void)fprintf(stderr, "fault reaction outside a guarded call, in %s\n", where);
    abort();
}
