/*
 * host_fault.c — the fault reaction for the host image check (tools/check_images.py). Host only:
 * never compiled into a program for the chip.
 */
#include <stdlib.h>

#include "fault.h"

/**
 * @brief Fault reaction of the host library: stops the host process.
 *
 * Why it exists: tools/check_images.py compiles common/image_check.c and common/crc32.c into a
 * library (libimagecheck.so), and image_check.c calls fault_impossible_value (common/fault.h)
 * for a value that cannot exist. A library with that function undefined does not link on this
 * computer. Every program on the board defines its own reaction (see common/fault.h), and the
 * unit tests define theirs (test/controller/unit), so this file lives outside src/ (code rule
 * A10b, owner, 13 Oct 2026) and is linked by check_images.py alone.
 *
 * @param[in]     where  the function that found the impossible value (not used)
 * @note Does not return: abort() ends the process.
 */
void fault_impossible_value(const char *where)
{
    (void)where;   /* nothing to report it to in a host library: the process stops */
    abort();
}
