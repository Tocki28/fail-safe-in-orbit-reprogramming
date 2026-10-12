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
 *   target = 0x0431                          0x0B built for another controller
 *   header Length + 64 + 128 = image size    0x08 damaged
 *   CRC-32 of the body = body check value    0x08 damaged
 *   Signature A with key A                   0x09 not from the operator
 *   Signature B with key B                   0x09 not from the operator
 * The header has no check value of its own: damage to it shows as whichever check it breaks
 * first, and at the latest as a signature (0x09).
 * Not checked here (bench): minimum version (0x0A, REQ-045; SET LIMITS is not built) and the
 * slot the image is linked for (0x0E; the controller checks it, src/controller/common/
 * image_check.c).
 */
#ifndef FSIR_UPDATER_IMAGE_CHECKER_HPP
#define FSIR_UPDATER_IMAGE_CHECKER_HPP

#include <cstddef>
#include <cstdint>

struct evp_pkey_st;   // OpenSSL's EVP_PKEY, kept out of this header

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

  ImageVerdict check(const uint8_t * image, size_t size) const;

private:
  evp_pkey_st * key_a_;
  evp_pkey_st * key_b_;
};

#endif  // FSIR_UPDATER_IMAGE_CHECKER_HPP
