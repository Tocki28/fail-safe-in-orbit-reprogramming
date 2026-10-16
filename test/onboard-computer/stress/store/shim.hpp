/*
 * shim.hpp — a counting and cutting shim for the file-system calls of the code under test.
 *
 * What it is: the test binary defines open, close, write, pwrite, fsync, rename, unlink and
 * ftruncate itself (shim.cpp), so the calls made by the updater's objects linked into the binary
 * reach these wrappers first; each wrapper counts the call and then calls the real one through
 * dlsym(RTLD_NEXT). Why: to stop a write at an exact step (the model of a power cut) without any
 * change to src/ (code rules A10b). Calls made inside the C library (stdio, std::filesystem) do
 * not pass through here.
 *
 * Modes: kOff counts nothing; kRecord counts and notes every call; kCut counts and ends the
 * process with _exit(kCutExit) at the chosen step, before the call, after it, or (for a write
 * of more than one byte) after half of its bytes.
 */
#ifndef FSIR_STRESS_STORE_SHIM_HPP
#define FSIR_STRESS_STORE_SHIM_HPP

#include <cstddef>
#include <cstdint>

namespace shim
{

// Where in a call the cut falls.
enum class When : uint8_t
{
  kBefore,   // the process ends before the call is made
  kAfter,    // the process ends right after the call returned
  kHalf,     // a write: half of its bytes are written, then the process ends
};

// Exit status of a process ended by the shim. 77: any value that the code under test and
// _exit's callers do not use; chosen here, no source.
constexpr int kCutExit = 77;

// Room for the calls of one record: durable_record.cpp makes 8 calls in a good run
// (open, write, fsync, close, rename, open, fsync, close); 64 leaves room for more.
constexpr size_t kCallsMax = 64;
// Longest call name and path kept in a note.
constexpr size_t kNameLen = 16;
constexpr size_t kPathLen = 160;

// One counted call.
struct Call
{
  char name[kNameLen];
  char path[kPathLen];
  size_t bytes;   // for a write: the byte count asked for; else 0
};

void off();
void startRecording();
void startCut(size_t step, When when);
size_t callCount();
Call callAt(size_t index);

}  // namespace shim

#endif  // FSIR_STRESS_STORE_SHIM_HPP
