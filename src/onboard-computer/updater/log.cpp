#include "log.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>

#include "clock.hpp"

namespace
{

uint64_t g_start_ms = 0;
bool g_verbose = false;

constexpr size_t kLineBytes = 512;
constexpr uint64_t kMsPerSecond = 1000;

/**
 * @brief Prints the elapsed-time prefix of a log line to stderr.
 *
 * Elapsed time is counted from the start time given to logging::start(), as seconds.milliseconds.
 * @return nothing
 */
void stamp()
{
  uint64_t elapsed = clocks::nowMs() - g_start_ms;
  (void)std::fprintf(stderr, "[%6llu.%03llu] ",
    static_cast<unsigned long long>(elapsed / kMsPerSecond),
    static_cast<unsigned long long>(elapsed % kMsPerSecond));
}

}  // namespace

namespace logging
{

/**
 * @brief Sets the log start time and the frame-printing switch.
 *
 * Later log lines show their time relative to now_ms.
 *
 * @param[in]    now_ms  monotonic time of the start, milliseconds
 * @param[in]    print_frames  true: print every frame as hex bytes
 * @return nothing
 */
void start(uint64_t now_ms, bool print_frames)
{
  g_start_ms = now_ms;
  g_verbose = print_frames;
}

/**
 * @brief Tells whether frames are printed.
 *
 * Reflects the print_frames value given to logging::start().
 * @return true: frames are printed; false: they are not
 */
bool verbose()
{
  return g_verbose;
}

/**
 * @brief Prints one time-stamped line to stderr.
 *
 * Format into one buffer first, so the line reaches stderr in one write and cannot be
 * split by another program writing to the same terminal. Longer lines are cut at the buffer size.
 *
 * @param[in]    format  printf-style format string
 * @param[in]    ...  values for the format
 * @return nothing
 */
void line(const char * format, ...)
{
  // Format into one buffer first, so the line reaches stderr in one write and cannot be
  // split by another program writing to the same terminal.
  std::array<char, kLineBytes> text{};
  va_list args;
  va_start(args, format);
  (void)std::vsnprintf(text.data(), text.size(), format, args);
  va_end(args);
  stamp();
  (void)std::fprintf(stderr, "%s\n", text.data());
}

/**
 * @brief Prints the bytes of one frame as hex when frame printing is on.
 *
 * Prints nothing unless logging::start() was called with print_frames set.
 *
 * @param[in]    what  label printed before the byte count
 * @param[in]    bytes  frame bytes
 * @param[in]    size  number of bytes
 * @return nothing
 */
void frame(const char * what, const uint8_t * bytes, size_t size)
{
  if (!g_verbose) {
    return;
  }
  stamp();
  (void)std::fprintf(stderr, "%s (%zu bytes):", what, size);
  for (size_t i = 0; i < size; i++) {
    (void)std::fprintf(stderr, " %02x", bytes[i]);
  }
  (void)std::fprintf(stderr, "\n");
}

}  // namespace logging
