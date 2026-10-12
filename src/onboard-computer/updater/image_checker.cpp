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

ImageVerdict passed(uint8_t kind, uint32_t version)
{
  return ImageVerdict{answer::kSuccess, "passed", kind, version};
}

ImageVerdict refused(uint8_t code, const char * reason)
{
  return ImageVerdict{code, reason, 0, 0};
}

bool magicOk(const uint8_t * image)
{
  return image[IMAGE_OFFSET_MAGIC] == IMAGE_MAGIC_0 &&
         image[IMAGE_OFFSET_MAGIC + 1] == IMAGE_MAGIC_1 &&
         image[IMAGE_OFFSET_MAGIC + 2] == IMAGE_MAGIC_2 &&
         image[IMAGE_OFFSET_MAGIC + 3] == IMAGE_MAGIC_3;
}

bool kindKnown(uint8_t kind)
{
  return kind == IMAGE_KIND_BOOTLOADER || kind == IMAGE_KIND_MAIN ||
         kind == IMAGE_KIND_LAST_RESORT;
}

// Pure Ed25519: no digest is named, the message goes in whole [OpenSSL EVP_SIGNATURE-ED25519(7);
// EVP_DigestVerify(3)]. EVP_DigestVerify returns 1 only for a valid signature; 0 for an
// invalid one, below 0 for an error. Both count as "not valid".
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

ImageChecker::ImageChecker(const uint8_t * key_a, const uint8_t * key_b)
: key_a_(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key_a, config::kPublicKeyBytes)),
  key_b_(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key_b, config::kPublicKeyBytes))
{
}

ImageChecker::~ImageChecker()
{
  EVP_PKEY_free(key_a_);   // accepts nullptr
  EVP_PKEY_free(key_b_);
}

bool ImageChecker::ready() const
{
  return key_a_ != nullptr && key_b_ != nullptr;
}

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
