/*
 * log.hpp — one line per event on stderr, stamped with seconds since the program started.
 *
 * Bench limit: the log goes to stderr only. Keeping a record of what was done (REQ-061 "record
 * what it did") on the onboard computer and sending it down is the report store's later work.
 */
#ifndef FSIR_UPDATER_LOG_HPP
#define FSIR_UPDATER_LOG_HPP

#include <cstddef>
#include <cstdint>

namespace logging
{

// Sets the time that the stamps count from, and whether frames are printed (--verbose).
void start(uint64_t now_ms, bool print_frames);

bool verbose();

// printf-style; a newline is added.
void line(const char * format, ...) __attribute__((format(printf, 1, 2)));

// The bytes of one frame in hex, only with --verbose.
void frame(const char * what, const uint8_t * bytes, size_t size);

}  // namespace logging

#endif  // FSIR_UPDATER_LOG_HPP
