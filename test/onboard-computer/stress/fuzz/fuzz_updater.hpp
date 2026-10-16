/*
 * fuzz_updater.hpp — fuzz target 3: the payloads behind the frames, through the real interface.
 *
 * An Updater is driven in-process as test/onboard-computer/unit/test_updater.cpp drives it: a fake
 * Transport for data link A, a loopback UDP port for the ground, and the clock moved by the test.
 * No change to src/ is needed.
 */
#ifndef FSIR_FUZZ_UPDATER_HPP
#define FSIR_FUZZ_UPDATER_HPP

#include "fuzz_common.hpp"

namespace fuzz
{

// Target "updater": sessions of fuzzed ground command datagrams and fuzzed controller frames
// against a fresh Updater each; the index of an input is the session number.
int runUpdater(Report & r);

}  // namespace fuzz

#endif  // FSIR_FUZZ_UPDATER_HPP
