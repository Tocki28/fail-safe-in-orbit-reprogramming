/*
 * image_checker.hpp — the image half of the "command and image checker" (Architecture
 * Sec. 6.0): decides whether a firmware image may be stored and delivered (REQ-043, REQ-044,
 * REQ-046, REQ-047).
 *
 * Image layout (Interfaces Sec. 5.5, Figures 5 and 6; src/controller/common/image_format.h):
 *   header (64) | body (Length) | Signature A (64) | Signature B (64)
 * Both signatures are pure Ed25519 [RFC 8032, Sec. 5.1.7] over header | body.
 *
 * Signatures are checked with OpenSSL's libcrypto (EVP interface). The controller (its
 * bootloader and its update receiver) checks them with Monocypher
 * (src/controller/common/image_check.c). REQ-130 asks for the two to be different code, so
 * that one defect cannot pass forged firmware through both checks (Architecture Sec. 5.4).
 * This file shares no code with image_check.c; only the format constants of image_format.h.
 *
 * Checks, cheapest first, in the order of the build brief, and the refusal reason each gives
 * (Interfaces Sec. 4.2):
 *   size can hold header and signatures      0x08 damaged
 *   proprietary string "FSIR"                0x08 damaged (not an image)
 *   format = 1                               0x08 damaged
 *   kind 0x01, 0x02 or 0x03                  0x06 impossible values
 *   target = the controller identity (limits)  0x0B built for another controller (REQ-046)
   version >= the minimum version (limits)  0x0A older than the minimum (REQ-045)
 *   header Length + 64 + 128 = image size    0x08 damaged
 *   CRC-32 of the body = body check value    0x08 damaged
 *   Signature A with key A                   0x09 not from the operator
 *   Signature B with key B                   0x09 not from the operator
 * The header has no check value of its own: damage to it shows as whichever check it breaks
 * first, and at the latest as a signature (0x09).
 * The identity and the minimum version are the "limits" (ImageLimits), set by SET LIMITS (owner
 * decision 24, 15 Oct 2026; REQ-053). The first line of check() refuses everything with 0x0A when
 * the limits are not usable (a damaged limits record: fail closed, TBR).
 * Not checked here (bench): the slot the image is linked for (0x0E; the controller checks it,
 * src/controller/common/image_check.c).
 */
#ifndef FSIR_UPDATER_IMAGE_CHECKER_HPP
#define FSIR_UPDATER_IMAGE_CHECKER_HPP

#include <cstddef>
#include <cstdint>

struct evp_pkey_st;   // OpenSSL's EVP_PKEY, kept out of this header

// The two limits an image must meet (REQ-045, REQ-046, REQ-053; owner decision 24, 15 Oct 2026).
struct ImageLimits
{
  uint32_t minimum_version;   // an image with a lower header version is refused 0x0A
  uint16_t identity;          // an image whose header target differs is refused 0x0B
  bool usable;                // false: the limits could not be read; every image is refused 0x0A
};

// The limits of a first start, with no record: minimum version config::kDefaultMinimumVersion
// and identity config::kDefaultControllerIdentity, usable.
ImageLimits defaultImageLimits();

struct ImageVerdict
{
  uint8_t code;          // 0x00 passed, else the refusal reason (protocol.hpp)
  const char * reason;   // for the log, e.g. "signature A"
  uint8_t kind;          // header fields, valid once the header checks passed
  uint32_t version;
};

class ImageChecker
{
public:
  // key_a and key_b: 32-byte Ed25519 public keys [RFC 8032, Sec. 5.1.5].
  ImageChecker(const uint8_t * key_a, const uint8_t * key_b);
  ~ImageChecker();
  ImageChecker(const ImageChecker &) = delete;
  ImageChecker & operator=(const ImageChecker &) = delete;

  // Both keys were accepted by OpenSSL.
  bool ready() const;

  // The checks against the limits given (the production calls: the store's, UploadStore::limits()).
  ImageVerdict check(const uint8_t * image, size_t size, const ImageLimits & limits) const;

  // The same with defaultImageLimits(). For callers that have no limits to apply (the tests of
  // the other checks); the updater itself always passes the stored limits.
  ImageVerdict check(const uint8_t * image, size_t size) const;

private:
  evp_pkey_st * key_a_;
  evp_pkey_st * key_b_;
};

#endif  // FSIR_UPDATER_IMAGE_CHECKER_HPP
