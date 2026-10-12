#include "options.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.hpp"

void printUsage(const char * program)
{
  (void)std::fprintf(stderr,
    "usage: %s [--device PATH] [--udp-port N] [--data-dir DIR]\n"
    "          [--key-a FILE] [--key-b FILE] [--beat-s N] [--verbose]\n"
    "  --device PATH   data link A serial device (default %s)\n"
    "  --udp-port N    UDP port for the ground, on all interfaces (default %u)\n"
    "  --data-dir DIR  where uploaded images are kept (default %s)\n"
    "  --key-a FILE    public key A: a file of exactly 32 bytes (default: built in, keys.h)\n"
    "  --key-b FILE    public key B: a file of exactly 32 bytes (default: built in, keys.h)\n"
    "  --beat-s N      GET STATUS to the controller every N s when the link is idle,\n"
    "                  0..%u, 0 = off (default %u)\n"
    "  --verbose       print every frame sent and received\n",
    program, config::kDefaultDevice, config::kDefaultUdpPort, config::kDefaultDataDir,
    config::kMaxBeatS, config::kDefaultBeatS);
}

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

bool parseOptions(int argc, const char * const * argv, Options & options)
{
  options.device = config::kDefaultDevice;
  options.udp_port = config::kDefaultUdpPort;
  options.data_dir = config::kDefaultDataDir;
  options.key_a_file = nullptr;
  options.key_b_file = nullptr;
  options.beat_s = config::kDefaultBeatS;
  options.verbose = false;

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
  return true;
}

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
