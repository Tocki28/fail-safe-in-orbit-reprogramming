/*
 * fuzz_frames.hpp — fuzz targets 1 and 2: the ground frame decoder (frame::decode and
 * frame::decodeAnswer) and the data-link frame parser (frame::Parser::feed).
 */
#ifndef FSIR_FUZZ_FRAMES_HPP
#define FSIR_FUZZ_FRAMES_HPP

#include "fuzz_common.hpp"

namespace fuzz
{

// Target "frame": one datagram at a time against the pinned C reference (commit 5c0b5f8) and a
// spec decoder of Interfaces Sec. 4.0. Returns the number of findings.
int runFrameDecode(Report & r);

// Target "parser": byte streams with gaps through frame::Parser and the C reference parser.
int runFrameParser(Report & r);

}  // namespace fuzz

#endif  // FSIR_FUZZ_FRAMES_HPP
