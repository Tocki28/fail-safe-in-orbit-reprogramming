/*
 * a3.hpp — Part A.3 of the stress audit: a power cut at every file-system step of a durable
 * record write (DurableRecord::write, clock_record::save, limits_record::save).
 */
#ifndef FSIR_STRESS_STORE_A3_HPP
#define FSIR_STRESS_STORE_A3_HPP

#include <cstdio>
#include <string>

namespace a3
{

int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings);

}  // namespace a3

#endif  // FSIR_STRESS_STORE_A3_HPP
