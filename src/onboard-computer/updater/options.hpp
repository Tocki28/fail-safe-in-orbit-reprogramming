/*
 * options.hpp — the command line. In its own file, not in main.cpp, so the unit tests can call
 * it.
 */
#ifndef FSIR_UPDATER_OPTIONS_HPP
#define FSIR_UPDATER_OPTIONS_HPP

#include <cstddef>
#include <cstdint>

struct Options
{
  const char * device;       // data link A's serial device
  uint16_t udp_port;         // the ground link's UDP port
  const char * data_dir;     // where images are stored
  const char * key_a_file;   // 32-byte public key A; nullptr: the key built in from keys.h
  const char * key_b_file;   // 32-byte public key B; nullptr: the key built in from keys.h
  uint32_t beat_s;           // GET STATUS to the controller every beat_s when idle; 0: off
  bool verbose;              // print every frame
  const char * check_image;  // --check-image FILE: check-only mode (image_file_check.hpp); nullptr: run as the updater
  const char * check_kind;   // --kind: bootloader, main or last-resort; used with --check-image
  const char * restore_script;   // --restore-script: restore.sh, run in mode "reset" when a trial overruns; nullptr: next to the program (defaultRestoreScript)
};

void printUsage(const char * program);

// Text to a whole number between min and max. False if not a number, extra characters, or
// out of range; value is then unchanged.
[[nodiscard]] bool parseNumber(const char * text, unsigned long min, unsigned long max,
  unsigned long & value);

// Sets the defaults, then reads every word. False if a word is not understood.
[[nodiscard]] bool parseOptions(int argc, const char * const * argv, Options & options);

// The restart script next to the program file: <directory of the program>/restore.sh, written to
// out (room bytes). On Linux the program file is read from /proc/self/exe, else from program
// (argv[0]). False if the directory is not known (argv[0] without a slash and no /proc) or the
// path does not fit.
[[nodiscard]] bool defaultRestoreScript(const char * program, char * out, size_t room);

// Reads a public key file: exactly 32 bytes, the raw Ed25519 public key [RFC 8032, Sec. 5.1.5].
[[nodiscard]] bool readKeyFile(const char * path, uint8_t * key);

#endif  // FSIR_UPDATER_OPTIONS_HPP
