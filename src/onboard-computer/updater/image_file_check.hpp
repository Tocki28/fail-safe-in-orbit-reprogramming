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
[[nodiscard]] bool checkImageFile(const char * path, uint8_t kind, const ImageChecker & checker,
  char * line, size_t line_bytes);

#endif  // FSIR_UPDATER_IMAGE_FILE_CHECK_HPP
