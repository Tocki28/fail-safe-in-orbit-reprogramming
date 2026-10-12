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

void start(uint64_t now_ms, bool print_frames)
{
  g_start_ms = now_ms;
  g_verbose = print_frames;
}

bool verbose()
{
  return g_verbose;
}

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
