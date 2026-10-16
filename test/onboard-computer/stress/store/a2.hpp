/*
 * a2.hpp — Part A.2 of the stress audit: the report store cut at every byte of its last records.
 */
#ifndef FSIR_STRESS_STORE_A2_HPP
#define FSIR_STRESS_STORE_A2_HPP

#include <cstdio>
#include <string>

namespace a2
{

int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings);

}  // namespace a2

#endif  // FSIR_STRESS_STORE_A2_HPP
