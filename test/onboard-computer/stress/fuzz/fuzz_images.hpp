/*
 * fuzz_images.hpp — fuzz target 4: the image header and image (ImageChecker::check),
 * parseKindName
 * and checkImageFile.
 */
#ifndef FSIR_FUZZ_IMAGES_HPP
#define FSIR_FUZZ_IMAGES_HPP

#include "fuzz_common.hpp"

namespace fuzz
{

// Target "image": mutated signed images through ImageChecker::check, plus an exhaustive pass of
// every single-bit flip, truncation and extension of every valid seed.
int runImage(Report & r);

// Target "imagefile": mutated images written to a file and judged by checkImageFile, with
// parseKindName on random words, and special files (missing, directory, empty, too large).
int runImageFile(Report & r);

}  // namespace fuzz

#endif  // FSIR_FUZZ_IMAGES_HPP
