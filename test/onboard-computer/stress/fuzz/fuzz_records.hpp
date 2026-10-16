/*
 * fuzz_records.hpp — fuzz targets 5 and 6: the durable record (DurableRecord::read, with
 * clock_record::load and limits_record::load on top) and the report store (ReportStore::load).
 */
#ifndef FSIR_FUZZ_RECORDS_HPP
#define FSIR_FUZZ_RECORDS_HPP

#include "fuzz_common.hpp"

namespace fuzz
{

// Target "records": fuzzed record files through DurableRecord::read and the two loaders.
int runRecords(Report & r);

// Target "reports": fuzzed store files through ReportStore::load, then a read-back.
int runReports(Report & r);

}  // namespace fuzz

#endif  // FSIR_FUZZ_RECORDS_HPP
