#include "image_checker.hpp"

// OpenSSL's headers use C casts in inline functions. On this Mac they are found through
// -I/usr/local/include (Homebrew's links), as ordinary headers, so their casts would stop the
// build under -Wold-style-cast -Werror. The warnings are switched off for this include only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <openssl/evp.h>
#pragma GCC diagnostic pop

#include "bytes.hpp"
#include "config.hpp"
#include "crc32.hpp"
#include "protocol.hpp"

namespace
{

/**
 * @brief Builds the verdict of an image that passed every check.
 *
 * @param[in] kind  image kind from the header
 * @param[in] version  image version from the header
 * @return verdict with code answer::kSuccess, reason "passed", and the given kind and version
 */
ImageVerdict passed(uint8_t kind, uint32_t version)
{
  return ImageVerdict{answer::kSuccess, "passed", kind, version};
}

/**
 * @brief Builds the verdict of a refused image.
 *
 * @param[in] code  refusal reason (protocol.hpp)
 * @param[in] reason  short text for the log
 * @return verdict with the given code and reason, kind 0 and version 0
 */
ImageVerdict refused(uint8_t code, const char * reason)
{
  return ImageVerdict{code, reason, 0, 0};
}

/**
 * @brief Checks the proprietary string at the start of the header.
 *
 * @param[in] image  the image; at least the header must be readable
 * @return true: the four magic bytes match; false: otherwise
 */
bool magicOk(const uint8_t * image)
{
  return image[IMAGE_OFFSET_MAGIC] == IMAGE_MAGIC_0 &&
         image[IMAGE_OFFSET_MAGIC + 1] == IMAGE_MAGIC_1 &&
         image[IMAGE_OFFSET_MAGIC + 2] == IMAGE_MAGIC_2 &&
         image[IMAGE_OFFSET_MAGIC + 3] == IMAGE_MAGIC_3;
}

/**
 * @brief Tells whether an image kind is one of the three defined.
 *
 * Kinds: bootloader, main, last-resort (image_format.h).
 *
 * @param[in] kind  kind byte from the header
 * @return true: the kind is a bootloader, main or last-resort image; false: any other value
 */
bool kindKnown(uint8_t kind)
{
  return kind == IMAGE_KIND_BOOTLOADER || kind == IMAGE_KIND_MAIN ||
         kind == IMAGE_KIND_LAST_RESORT;
}

/**
 * @brief Checks one Ed25519 signature over a message.
 *
 * Pure Ed25519: no digest is named, the message goes in whole [OpenSSL EVP_SIGNATURE-ED25519(7);
 * EVP_DigestVerify(3)]. EVP_DigestVerify returns 1 only for a valid signature; 0 for an
 * invalid one, below 0 for an error. Both count as "not valid".
 *
 * @param[in] key  public key to verify with
 * @param[in] signature  the signature, config::kSignatureBytes bytes
 * @param[in] message  the signed bytes (header and body)
 * @param[in] size  size of message, bytes
 * @return true: the signature is valid for message under key; false: invalid, or an OpenSSL error
 */
bool signatureOk(EVP_PKEY * key, const uint8_t * signature, const uint8_t * message, size_t size)
{
  EVP_MD_CTX * context = EVP_MD_CTX_new();
  if (context == nullptr) {
    return false;
  }
  bool ok = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestVerify(context, signature, config::kSignatureBytes, message, size) == 1;
  EVP_MD_CTX_free(context);
  return ok;
}

}  // namespace

/**
 * @brief Builds a checker from the two public keys.
 *
 * key_a and key_b: 32-byte Ed25519 public keys [RFC 8032, Sec. 5.1.5]. A key OpenSSL does not
 * accept leaves ready() false.
 *
 * @param[in] key_a  public key A, config::kPublicKeyBytes bytes
 * @param[in] key_b  public key B, config::kPublicKeyBytes bytes
 * @return nothing (constructor)
 */
ImageChecker::ImageChecker(const uint8_t * key_a, const uint8_t * key_b)
: key_a_(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key_a, config::kPublicKeyBytes)),
  key_b_(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key_b, config::kPublicKeyBytes))
{
}

/**
 * @brief Frees the two keys.
 * @return nothing (destructor)
 */
ImageChecker::~ImageChecker()
{
  EVP_PKEY_free(key_a_);   // accepts nullptr
  EVP_PKEY_free(key_b_);
}

/**
 * @brief Tells whether both keys were accepted.
 *
 * Both keys were accepted by OpenSSL.
 * @return true: both keys are loaded; false: at least one was refused
 */
bool ImageChecker::ready() const
{
  return key_a_ != nullptr && key_b_ != nullptr;
}

/**
 * @brief Decides whether a firmware image may be stored and delivered.
 *
 * Runs the checks in the order listed in image_checker.hpp, cheapest first, and stops at the first
 * that fails. The verdict carries the refusal code of Interfaces Sec. 4.2 and a reason for the log.
 * The header Length is checked against the size before any byte of the body is read.
 *
 * @param[in] image  the whole image: header, body, Signature A, Signature B
 * @param[in] size  size of image, bytes
 * @return verdict: code answer::kSuccess with the header's kind and version if every check passed; otherwise the refusal code and reason
 */
ImageVerdict ImageChecker::check(const uint8_t * image, size_t size) const
{
  if (!ready()) {
    return refused(answer::kNotFromOperator, "no checking keys");
  }
  // Too short to hold a header and two signatures; also guards every read below.
  if (size <= config::kImageOverheadBytes || size > config::kMaxImageBytes) {
    return refused(answer::kImageDamaged, "size");
  }
  if (!magicOk(image)) {
    return refused(answer::kImageDamaged, "proprietary string");
  }
  if (image[IMAGE_OFFSET_FORMAT] != IMAGE_FORMAT_VERSION) {
    return refused(answer::kImageDamaged, "format");
  }
  uint8_t kind = image[IMAGE_OFFSET_KIND];
  if (!kindKnown(kind)) {
    return refused(answer::kImpossible, "kind");
  }
  if (bytes::readU16(&image[IMAGE_OFFSET_TARGET]) != IMAGE_TARGET_CONTROLLER) {
    return refused(answer::kOtherController, "target");
  }
  // Length is checked against the size before any byte of the body is read.
  uint32_t body = bytes::readU32(&image[IMAGE_OFFSET_LENGTH]);
  if (static_cast<size_t>(body) != size - config::kImageOverheadBytes) {
    return refused(answer::kImageDamaged, "length");
  }
  const uint8_t * body_start = &image[config::kImageHeaderBytes];
  if (crc::crc32(body_start, body) != bytes::readU32(&image[IMAGE_OFFSET_BODY_CHECK])) {
    return refused(answer::kImageDamaged, "body check value");
  }

  size_t signed_bytes = config::kImageHeaderBytes + body;
  const uint8_t * signature_a = &image[signed_bytes];
  const uint8_t * signature_b = signature_a + config::kSignatureBytes;
  if (!signatureOk(key_a_, signature_a, image, signed_bytes)) {
    return refused(answer::kNotFromOperator, "signature A");
  }
  if (!signatureOk(key_b_, signature_b, image, signed_bytes)) {
    return refused(answer::kNotFromOperator, "signature B");
  }
  return passed(kind, bytes::readU32(&image[IMAGE_OFFSET_VERSION]));
}
