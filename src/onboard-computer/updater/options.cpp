#include "options.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "config.hpp"

/**
 * @brief Prints the command-line usage to stderr.
 *
 * Lists every option with its default from config.hpp.
 *
 * @param[in] program  program name, shown in the first line
 * @return nothing
 */
void printUsage(const char * program)
{
  (void)std::fprintf(stderr,
    "usage: %s [--device PATH] [--udp-port N] [--data-dir DIR]\n"
    "          [--key-a FILE] [--key-b FILE] [--beat-s N] [--restore-script PATH] [--verbose]\n"
    "       %s --check-image FILE --kind bootloader|main|last-resort [--key-a FILE] [--key-b FILE]\n"
    "  --device PATH   data link A serial device (default %s)\n"
    "  --udp-port N    UDP port for the ground, on all interfaces (default %u)\n"
    "  --data-dir DIR  where uploaded images are kept (default %s)\n"
    "  --key-a FILE    public key A: a file of exactly 32 bytes (default: built in, keys.h)\n"
    "  --key-b FILE    public key B: a file of exactly 32 bytes (default: built in, keys.h)\n"
    "  --beat-s N      GET STATUS to the controller every N s when the link is idle,\n"
    "                  0..%u, 0 = off (default %u)\n"
    "  --restore-script PATH  restore.sh, run as 'restore.sh reset' to restart the controller when a\n"
    "                  trial runs past its time (default: %s next to this program)\n"
    "  --verbose       print every frame sent and received\n"
    "  --check-image FILE  check-only mode: run the checks of an upload on FILE and exit;\n"
    "                  0 = passed, 1 = refused (one line says why), 2 = usage, 3 = keys unusable\n"
    "  --kind K        the kind FILE must be: bootloader, main or last-resort (with --check-image)\n",
    program, program, config::kDefaultDevice, config::kDefaultUdpPort, config::kDefaultDataDir,
    config::kMaxBeatS, config::kDefaultBeatS, config::kRestoreScriptName);
}

/**
 * @brief Reads text as a whole number between min and max.
 *
 * Text to a whole number between min and max. False if not a number, extra characters, or
 * out of range; value is then unchanged. A sign is rejected.
 *
 * @param[in] text  decimal digits only; may be nullptr
 * @param[in] min  smallest accepted value
 * @param[in] max  largest accepted value
 * @param[out] value  the number; set only when true is returned
 * @return true: text is a number in [min, max] and value holds it; false: otherwise
 */
bool parseNumber(const char * text, unsigned long min, unsigned long max, unsigned long & value)
{
  if (text == nullptr || min > max) {
    return false;
  }
  errno = 0;
  char * end = nullptr;
  unsigned long parsed = std::strtoul(text, &end, 10);
  // Reject: too big, no digits, extra characters, a sign, or out of range.
  if (errno != 0 || end == text || *end != '\0' || text[0] == '-' || text[0] == '+' ||
      parsed < min || parsed > max) {
    return false;
  }
  value = parsed;
  return true;
}

/**
 * @brief Sets the defaults, then reads every word.
 *
 * Sets the defaults, then reads every word. False if a word is not understood. The UDP port is
 * accepted from 1 to 65535 and the beat from 0 to config::kMaxBeatS. --check-image and --kind
 * go together (check-only mode, image_file_check.hpp): one without the other is not understood.
 *
 * @param[in] argc  number of words in argv
 * @param[in] argv  the words; argv[0] is the program name and is skipped
 * @param[out] options  the options read, defaults for those not given
 * @return true: every word was understood; false: an unknown word, a missing value or a bad one
 */
bool parseOptions(int argc, const char * const * argv, Options & options)
{
  options.device = config::kDefaultDevice;
  options.udp_port = config::kDefaultUdpPort;
  options.data_dir = config::kDefaultDataDir;
  options.key_a_file = nullptr;
  options.key_b_file = nullptr;
  options.beat_s = config::kDefaultBeatS;
  options.verbose = false;
  options.check_image = nullptr;
  options.check_kind = nullptr;
  options.restore_script = nullptr;

  constexpr unsigned long kMaxPort = 65535;
  for (int i = 1; i < argc; i++) {   // bounded by argc
    const char * word = argv[i];
    bool has_value = (i + 1 < argc);
    unsigned long number = 0;

    if (std::strcmp(word, "--verbose") == 0) {
      options.verbose = true;
    } else if (std::strcmp(word, "--device") == 0 && has_value) {
      options.device = argv[++i];
    } else if (std::strcmp(word, "--data-dir") == 0 && has_value) {
      options.data_dir = argv[++i];
    } else if (std::strcmp(word, "--key-a") == 0 && has_value) {
      options.key_a_file = argv[++i];
    } else if (std::strcmp(word, "--key-b") == 0 && has_value) {
      options.key_b_file = argv[++i];
    } else if (std::strcmp(word, "--check-image") == 0 && has_value) {
      options.check_image = argv[++i];
    } else if (std::strcmp(word, "--restore-script") == 0 && has_value) {
      options.restore_script = argv[++i];
    } else if (std::strcmp(word, "--kind") == 0 && has_value) {
      options.check_kind = argv[++i];
    } else if (std::strcmp(word, "--udp-port") == 0 && has_value &&
               parseNumber(argv[++i], 1, kMaxPort, number)) {
      options.udp_port = static_cast<uint16_t>(number);
    } else if (std::strcmp(word, "--beat-s") == 0 && has_value &&
               parseNumber(argv[++i], 0, config::kMaxBeatS, number)) {
      options.beat_s = static_cast<uint32_t>(number);
    } else {
      return false;   // unknown word, a missing value, or a bad one
    }
  }
  // The check-only mode needs both words, and --kind means nothing without --check-image.
  if ((options.check_image == nullptr) != (options.check_kind == nullptr)) {
    return false;
  }
  return true;
}

/**
 * @brief Reads a public key file.
 *
 * Reads a public key file: exactly 32 bytes, the raw Ed25519 public key [RFC 8032, Sec. 5.1.5].
 *
 * @param[in] path  file to read
 * @param[out] key  receives config::kPublicKeyBytes bytes; written only when true is returned
 * @return true: the file holds exactly one key and key holds it; false: the file cannot be opened or its size is not a key's
 */
bool readKeyFile(const char * path, uint8_t * key)
{
  std::FILE * file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  // Read one byte more than a key: a longer file is not a raw key.
  uint8_t extra[config::kPublicKeyBytes + 1] = {};
  size_t count = std::fread(extra, 1, sizeof extra, file);
  (void)std::fclose(file);
  if (count != config::kPublicKeyBytes) {
    return false;
  }
  std::memcpy(key, extra, config::kPublicKeyBytes);
  return true;
}

/**
 * @brief Builds the path of the restart script next to the program file.
 *
 * Decision H (accepted by the owner, 13 Oct 2026): the default is restore.sh in the directory of
 * the running program, so the deployment keeps the program, the script, OpenOCD and its scripts
 * together (restore.sh finds OpenOCD next to itself). On Linux the program file comes from
 * /proc/self/exe, which is right however the program was started; elsewhere argv[0] is used, which
 * is right only when it holds a slash.
 *
 * @param[in]     program  argv[0]
 * @param[out]    out      receives the path, terminated; written only when true is returned
 * @param[in]     room     size of out, in bytes
 * @return true: out holds the path; false: the directory is not known, or the path does not fit
 */
bool defaultRestoreScript(const char * program, char * out, size_t room)
{
  std::array<char, 512> self{};
  const char * path = nullptr;
  ssize_t n = ::readlink("/proc/self/exe", self.data(), self.size() - 1);
  if (n > 0) {
    self[static_cast<size_t>(n)] = '\0';
    path = self.data();
  } else if (program != nullptr && std::strchr(program, '/') != nullptr) {
    path = program;
  }
  if (path == nullptr) {
    return false;
  }
  const char * slash = std::strrchr(path, '/');
  if (slash == nullptr) {
    return false;
  }
  size_t dir = static_cast<size_t>(slash - path) + 1;   // keeps the slash
  size_t name = std::strlen(config::kRestoreScriptName);
  if (dir + name + 1 > room) {
    return false;
  }
  std::memcpy(out, path, dir);
  std::memcpy(out + dir, config::kRestoreScriptName, name + 1);
  return true;
}
