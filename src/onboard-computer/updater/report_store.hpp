/*
 * report_store.hpp — the report store (Architecture Sec. 6.0), bench part: checks every report
 * from the controller, numbers it (REQ-071), stamps it with the onboard computer clock time
 * (REQ-070), and keeps the last HEALTH report for DELIVER's trial check (REQ-050) and the log.
 *
 * Layout (protocol.hpp): Sec. 5.4 lists the report number and the onboard computer clock time
 * first and says the onboard computer adds them. The controller sends the whole layout with
 * those two fields at 0 (src/controller/firmware/health-monitor/health_monitor.h at commit
 * 5c0b5f8), so one layout travels on both links, and the onboard computer writes the two
 * fields in place. The
 * frame to the ground gets a new check value; the controller's own was checked on arrival
 * (REQ-079, by the frame parser).
 *
 * BENCH LIMITS: reports are not stored between contacts (REQ-074 to REQ-078); each is sent
 * to the ground at once, or dropped if no ground has spoken yet. The report number starts at 1
 * at every start of the program, so numbering does not survive a restart (REQ-077).
 */
#ifndef FSIR_UPDATER_REPORT_STORE_HPP
#define FSIR_UPDATER_REPORT_STORE_HPP

#include <cstddef>
#include <cstdint>

struct HealthReport
{
  uint32_t controller_time;
  uint8_t mode;
  uint8_t firmware_kind;
  uint32_t firmware_version;
  uint8_t slot;
  uint8_t failed_starts;
  uint16_t trial_left_s;
  uint8_t self_test;
};

class ReportStore
{
public:
  ReportStore();

  // Checks a report from the controller (length and kind, Sec. 5.4) and writes into out
  // (room bytes) the report for the ground: the same bytes with the report number and the
  // onboard computer clock time filled in. False if the report does not fit Sec. 5.4.
  bool accept(const uint8_t * payload, size_t length, uint32_t onboard_time_s, uint64_t now_ms,
    uint8_t * out, size_t room);

  bool haveHealth() const;
  const HealthReport & lastHealth() const;
  uint64_t lastHealthMs() const;   // when it arrived (clocks::nowMs)
  uint32_t lastNumber() const;     // the last number given; 0 before the first

private:
  void noteHealth(const uint8_t * payload, uint64_t now_ms);

  uint32_t next_number_;
  bool have_health_;
  HealthReport health_;
  uint64_t health_ms_;
};

const char * modeName(uint8_t mode);
const char * slotName(uint8_t slot);
const char * eventName(uint16_t code);   // Sec. 5.4 event table, for the log
const char * refusalName(uint8_t code);   // Sec. 4.2 refusal table (0x01 to 0x11) and the bench codes, for the log
void describeEvent(uint16_t code, const uint8_t * detail, size_t detail_length, char * out,
  size_t room);   // the log text of one EVENT, detail decoded
const char * checkResultName(uint8_t result);   // Sec. 5.4 check-result table (events 0x0002, 0x0003), for the log
const char * faultKindName(uint8_t kind);   // fault_codes.h FAULT_KIND_* (event 0x0001); null if not listed
const char * faultLocationName(uint8_t where);   // fault_codes.h FAULT_AT_* (event 0x0005); null if not listed

#endif  // FSIR_UPDATER_REPORT_STORE_HPP
