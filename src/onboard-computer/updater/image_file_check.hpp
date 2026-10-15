/*
 * image_file_check.hpp — the check-only mode of fsir-updater: run the image checks of an
 * upload on one file and say whether it passes, writing nothing and talking to nobody.
 *
 *   fsir-updater --check-image FILE --kind {bootloader|main|last-resort}
 *
 * Why it exists (REQ-118, Interfaces Sec. 5.5): the restore script writes images straight into
 * the controller's flash over SWD, past the controller's own checks. Before it writes anything
 * it asks this mode about every input, so that an image that would be refused at UPLOAD END
 * (damaged, built for another controller, wrong kind, signed with a wrong key) is refused
 * there too. It is the same ImageChecker as an upload uses (image_checker.hpp): both Ed25519
 * signatures, the body check value, the length, the target and the kind. Beyond the upload
 * checks it also demands that the file is the kind asked for and fits the flash region of that
 * kind (layout.h), because the restore script writes it into that region.
 *
 * Limits (owner decision 24, 15 Oct 2026): the check applies the minimum version and the
 * controller identity stored in the data directory's "limits" record when it can be read; else
 * minimum version 1 and identity 0x0431, and the result line says which (main.cpp; TBR, awaiting
 * the owner's acceptance).
 */
#ifndef FSIR_UPDATER_IMAGE_FILE_CHECK_HPP
#define FSIR_UPDATER_IMAGE_FILE_CHECK_HPP

#include <cstddef>
#include <cstdint>

#include "image_checker.hpp"

// Text of a --kind word to the kind byte of image_format.h. False for any other word.
[[nodiscard]] bool parseKindName(const char * name, uint8_t & kind);

// Reads FILE and runs the checker. Returns true only when the image passes and is of the
// kind asked for; line receives one line of text (no newline) with the reason either way.
// The limits (minimum version, controller identity: owner decision 24, 15 Oct 2026) are the ones
// given; the line ends with "; limits: <limits_note>, minimum version N, identity 0xNNNN".
[[nodiscard]] bool checkImageFile(const char * path, uint8_t kind, const ImageChecker & checker,
  const ImageLimits & limits, const char * limits_note, char * line, size_t line_bytes);

// The same with the default limits (defaultImageLimits()), noted "default limits, none given".
[[nodiscard]] bool checkImageFile(const char * path, uint8_t kind, const ImageChecker & checker,
  char * line, size_t line_bytes);

#endif  // FSIR_UPDATER_IMAGE_FILE_CHECK_HPP
