/*
 * a4.hpp — Part A.4 (report-number half) of the stress audit: what the report store does when the
 * report number passes 0xFFFFFFFF.
 */
#ifndef FSIR_STRESS_STORE_A4_HPP
#define FSIR_STRESS_STORE_A4_HPP

#include <cstdio>
#include <string>

namespace a4
{

int run(const std::string & workdir, FILE * log, FILE * csv, size_t & findings);

}  // namespace a4

#endif  // FSIR_STRESS_STORE_A4_HPP
