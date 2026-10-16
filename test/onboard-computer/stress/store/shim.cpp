/*
 * shim.cpp — the wrappers behind shim.hpp.
 *
 * The functions carry the assembler names of the C library's (_open, _write, ...), so the linker
 * binds the updater objects' calls to them: a symbol defined in the executable wins over one in
 * libSystem for references inside the executable. The real functions are found with
 * dlsym(RTLD_NEXT, ...), which starts the search after the executable.
 */
#include "shim.hpp"

#include <cstdarg>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{

// File descriptors above this are not given a path in the notes (a test binary opens few).
constexpr int kFdMax = 256;

enum class Mode : uint8_t
{
  kOff,
  kRecord,
  kCut,
};

struct State
{
  Mode mode;
  size_t count;
  size_t cut_step;
  shim::When when;
  shim::Call calls[shim::kCallsMax];
  char fd_path[kFdMax][shim::kPathLen];
};

State g_state{};

/**
 * @brief Finds the real C library function of a name, after this executable.
 *
 * @param[in]     name  the function's name
 * @return its address; null when not found
 */
void * nextSymbol(const char * name)
{
  return dlsym(RTLD_NEXT, name);
}

/**
 * @brief Copies a text into a fixed buffer, cut to fit, always ended with zero.
 *
 * @param[out]    out   the buffer
 * @param[in]     room  its size in bytes, at least 1
 * @param[in]     text  the text; null gives an empty text
 * @return nothing
 */
void copyText(char * out, size_t room, const char * text)
{
  out[0] = '\0';
  if (text == nullptr) {
    return;
  }
  std::strncpy(out, text, room - 1U);
  out[room - 1U] = '\0';
}

/**
 * @brief Path noted for a file descriptor, or an empty text.
 *
 * @param[in]     fd  the descriptor
 * @return the path given when it was opened; "" when unknown
 */
const char * pathOf(int fd)
{
  if (fd < 0 || fd >= kFdMax) {
    return "";
  }
  return g_state.fd_path[fd];
}

/**
 * @brief Counts one call, notes it, and ends the process when the cut is "before" this call.
 *
 * @param[in]     name   the call's name
 * @param[in]     path   the path it works on; may be null
 * @param[in]     bytes  for a write, the byte count; else 0
 * @return the call's number (1 for the first); 0 when the shim is off
 */
size_t enter(const char * name, const char * path, size_t bytes)
{
  if (g_state.mode == Mode::kOff) {
    return 0;
  }
  g_state.count++;
  size_t step = g_state.count;
  if (step <= shim::kCallsMax) {
    shim::Call & c = g_state.calls[step - 1U];
    copyText(c.name, sizeof(c.name), name);
    copyText(c.path, sizeof(c.path), path);
    c.bytes = bytes;
  }
  if (g_state.mode == Mode::kCut && step == g_state.cut_step &&
      g_state.when == shim::When::kBefore) {
    _exit(shim::kCutExit);
  }
  return step;
}

/**
 * @brief Ends the process when the cut is "after" the call just made.
 *
 * @param[in]     step  the number enter() gave; 0: shim off
 * @return nothing
 */
void leave(size_t step)
{
  if (step != 0U && g_state.mode == Mode::kCut && step == g_state.cut_step &&
      g_state.when == shim::When::kAfter) {
    _exit(shim::kCutExit);
  }
}

using OpenFn = int (*)(const char *, int, ...);
using CloseFn = int (*)(int);
using WriteFn = ssize_t (*)(int, const void *, size_t);
using PwriteFn = ssize_t (*)(int, const void *, size_t, off_t);
using FsyncFn = int (*)(int);
using RenameFn = int (*)(const char *, const char *);
using UnlinkFn = int (*)(const char *);
using FtruncateFn = int (*)(int, off_t);

}  // namespace

namespace shim
{

/**
 * @brief Switches the shim off: calls pass through uncounted.
 *
 * @return nothing
 */
void off()
{
  g_state.mode = Mode::kOff;
}

/**
 * @brief Starts counting and noting calls, from zero.
 *
 * @return nothing
 */
void startRecording()
{
  std::memset(&g_state, 0, sizeof(g_state));
  g_state.mode = Mode::kRecord;
}

/**
 * @brief Starts counting calls from zero and ends the process at one of them.
 *
 * @param[in]     step  the call number to cut at, 1 for the first call counted
 * @param[in]     when  before the call, after it, or after half of a write's bytes
 * @return nothing
 */
void startCut(size_t step, When when)
{
  std::memset(&g_state, 0, sizeof(g_state));
  g_state.mode = Mode::kCut;
  g_state.cut_step = step;
  g_state.when = when;
}

/**
 * @brief Number of calls counted since startRecording() or startCut().
 *
 * @return the count; calls past kCallsMax are counted but not noted
 */
size_t callCount()
{
  return g_state.count;
}

/**
 * @brief One noted call.
 *
 * @param[in]     index  0 for the first call; below callCount() and kCallsMax
 * @return the note; all zero when index is out of range
 */
Call callAt(size_t index)
{
  if (index >= g_state.count || index >= kCallsMax) {
    return Call{};
  }
  return g_state.calls[index];
}

}  // namespace shim

extern "C"
{

int shimOpen(const char * path, int flags, ...) __asm__("_open");
int shimClose(int fd) __asm__("_close");
ssize_t shimWrite(int fd, const void * data, size_t size) __asm__("_write");
ssize_t shimPwrite(int fd, const void * data, size_t size, off_t offset) __asm__("_pwrite");
int shimFsync(int fd) __asm__("_fsync");
int shimRename(const char * from, const char * to) __asm__("_rename");
int shimUnlink(const char * path) __asm__("_unlink");
int shimFtruncate(int fd, off_t length) __asm__("_ftruncate");

/**
 * @brief open() with the shim's counting; the file descriptor's path is noted.
 *
 * @param[in]     path   file to open
 * @param[in]     flags  open flags
 * @param[in]     ...    the mode (int) when flags has O_CREAT
 * @return the real open()'s result
 */
int shimOpen(const char * path, int flags, ...)
{
  int mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }
  size_t step = enter("open", path, 0);
  OpenFn real = reinterpret_cast<OpenFn>(nextSymbol("open"));
  int fd = real(path, flags, mode);
  if (step != 0U && fd >= 0 && fd < kFdMax) {
    copyText(g_state.fd_path[fd], sizeof(g_state.fd_path[fd]), path);
  }
  leave(step);
  return fd;
}

/**
 * @brief close() with the shim's counting.
 *
 * @param[in]     fd  descriptor
 * @return the real close()'s result
 */
int shimClose(int fd)
{
  size_t step = enter("close", pathOf(fd), 0);
  CloseFn real = reinterpret_cast<CloseFn>(nextSymbol("close"));
  int result = real(fd);
  leave(step);
  return result;
}

/**
 * @brief write() with the shim's counting; the "half" cut writes half the bytes and ends.
 *
 * @param[in]     fd    descriptor
 * @param[in]     data  bytes
 * @param[in]     size  number of bytes
 * @return the real write()'s result
 */
ssize_t shimWrite(int fd, const void * data, size_t size)
{
  size_t step = enter("write", pathOf(fd), size);
  WriteFn real = reinterpret_cast<WriteFn>(nextSymbol("write"));
  if (step != 0U && g_state.mode == Mode::kCut && step == g_state.cut_step &&
      g_state.when == shim::When::kHalf && size > 1U) {
    (void)real(fd, data, size / 2U);
    _exit(shim::kCutExit);
  }
  ssize_t result = real(fd, data, size);
  leave(step);
  return result;
}

/**
 * @brief pwrite() with the shim's counting; the "half" cut writes half the bytes and ends.
 *
 * @param[in]     fd      descriptor
 * @param[in]     data    bytes
 * @param[in]     size    number of bytes
 * @param[in]     offset  where in the file
 * @return the real pwrite()'s result
 */
ssize_t shimPwrite(int fd, const void * data, size_t size, off_t offset)
{
  size_t step = enter("pwrite", pathOf(fd), size);
  PwriteFn real = reinterpret_cast<PwriteFn>(nextSymbol("pwrite"));
  if (step != 0U && g_state.mode == Mode::kCut && step == g_state.cut_step &&
      g_state.when == shim::When::kHalf && size > 1U) {
    (void)real(fd, data, size / 2U, offset);
    _exit(shim::kCutExit);
  }
  ssize_t result = real(fd, data, size, offset);
  leave(step);
  return result;
}

/**
 * @brief fsync() with the shim's counting.
 *
 * @param[in]     fd  descriptor
 * @return the real fsync()'s result
 */
int shimFsync(int fd)
{
  size_t step = enter("fsync", pathOf(fd), 0);
  FsyncFn real = reinterpret_cast<FsyncFn>(nextSymbol("fsync"));
  int result = real(fd);
  leave(step);
  return result;
}

/**
 * @brief rename() with the shim's counting.
 *
 * @param[in]     from  old name
 * @param[in]     to    new name
 * @return the real rename()'s result
 */
int shimRename(const char * from, const char * to)
{
  size_t step = enter("rename", to, 0);
  RenameFn real = reinterpret_cast<RenameFn>(nextSymbol("rename"));
  int result = real(from, to);
  leave(step);
  return result;
}

/**
 * @brief unlink() with the shim's counting.
 *
 * @param[in]     path  file to remove
 * @return the real unlink()'s result
 */
int shimUnlink(const char * path)
{
  size_t step = enter("unlink", path, 0);
  UnlinkFn real = reinterpret_cast<UnlinkFn>(nextSymbol("unlink"));
  int result = real(path);
  leave(step);
  return result;
}

/**
 * @brief ftruncate() with the shim's counting.
 *
 * @param[in]     fd      descriptor
 * @param[in]     length  new length in bytes
 * @return the real ftruncate()'s result
 */
int shimFtruncate(int fd, off_t length)
{
  size_t step = enter("ftruncate", pathOf(fd), 0);
  FtruncateFn real = reinterpret_cast<FtruncateFn>(nextSymbol("ftruncate"));
  int result = real(fd, length);
  leave(step);
  return result;
}

}  // extern "C"
