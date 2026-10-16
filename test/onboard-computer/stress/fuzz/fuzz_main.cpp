/*
 * fuzz_main.cpp — entry point of the fuzz tool: picks the target named on the command line.
 */
#include <cstdio>
#include <cstdlib>
#include <string>

#include "fuzz_common.hpp"
#include "fuzz_frames.hpp"
#include "fuzz_images.hpp"
#include "fuzz_records.hpp"
#include "fuzz_updater.hpp"

/**
 * @brief Runs one fuzz target.
 *
 * Usage: fuzz TARGET --seed S --n N --out DIR --work DIR --ctrl-images DIR --test-images DIR
 * [--only INDEX], TARGET one of frame, parser, updater, image, imagefile, records, reports.
 * The Makefile sets FUZZ_START and FUZZ_COMMIT for the first log line and points stderr at a
 * file opened for appending.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments
 * @return 0: no finding; 1: findings; 2: usage error
 */
int main(int argc, char ** argv)
{
  fuzz::Options opt;
  if (!fuzz::parseOptions(argc, argv, opt)) {
    (void)std::fprintf(stderr, "usage: fuzz TARGET --seed S --n N --out DIR --work DIR "
      "--ctrl-images DIR --test-images DIR [--only INDEX]\n");
    return 2;
  }
  std::string cmd;
  for (int i = 0; i < argc; i++) {
    cmd += std::string(i > 0 ? " " : "") + argv[i];
  }
  fuzz::Report r;
  if (!fuzz::reportOpen(r, opt, cmd.c_str())) {
    (void)std::fprintf(stderr, "cannot open the log in %s\n", opt.out_dir.c_str());
    return 2;
  }
  int found = 0;
  if (opt.target == "frame") {
    found = fuzz::runFrameDecode(r);
  } else if (opt.target == "parser") {
    found = fuzz::runFrameParser(r);
  } else if (opt.target == "updater") {
    found = fuzz::runUpdater(r);
  } else if (opt.target == "image") {
    found = fuzz::runImage(r);
  } else if (opt.target == "imagefile") {
    found = fuzz::runImageFile(r);
  } else if (opt.target == "records") {
    found = fuzz::runRecords(r);
  } else if (opt.target == "reports") {
    found = fuzz::runReports(r);
  } else {
    (void)std::fprintf(r.log, "unknown target %s\n", opt.target.c_str());
    found = 1;
  }
  fuzz::reportClose(r);
  return (found > 0) ? 1 : 0;
}
