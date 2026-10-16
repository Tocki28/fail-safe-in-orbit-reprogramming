/*
 * report_store.hpp — the report store (Architecture Sec. 6.0): checks every report from the
 * controller, numbers it (REQ-071), stamps it with the onboard computer clock time (REQ-070),
 * keeps the last HEALTH report for DELIVER's trial check (REQ-050) and the log, and keeps every
 * report on disk between contacts (owner decision 29, 15 Oct 2026; REQ-074 to REQ-078).
 *
 * Layout (protocol.hpp): Sec. 5.4 lists the report number and the onboard computer clock time
 * first and says the onboard computer adds them. The controller sends the whole layout with
 * those two fields at 0 (src/controller/firmware/health-monitor/health_monitor.h at commit
 * 5c0b5f8), so one layout travels on both links, and the onboard computer writes the two
 * fields in place. The
 * frame to the ground gets a new check value; the controller's own was checked on arrival
 * (REQ-079, by the frame parser).
 *
 * THE STORE ON DISK (decision 29: "Report storage between contacts: 1 MB; events never dropped;
 * oldest HEALTH first; count and span reported"):
 *   - Every report accept() numbers (the controller's HEALTH and EVENT, and the onboard
 *     computer's own events) is also appended to the file config::kReportStoreName in the data
 *     directory, with its number and onboard time (REQ-074). The live path to the ground does not
 *     change: the updater still sends each report at once when a ground is known.
 *   - A contact is SEND REPORTS (Interfaces Sec. 5.1, row 0x0B) with a first report number. The
 *     updater answers DONE, sends EVENT 0x0104 when HEALTH reports were dropped (REQ-076), then
 *     every stored report numbered from the first number up to the last numbered before the
 *     contact: all EVENT reports first, then the HEALTH reports, each in number order (REQ-078).
 *     Stored reports numbered below the first number are deleted: the ground has them (TBR).
 *   - When a report would take the stored reports past config::kReportStoreBytes, the oldest
 *     HEALTH reports are dropped, as many as needed; events are never dropped (REQ-075). If only
 *     events are left, a new EVENT is still kept and the store passes its size; a new HEALTH is
 *     not stored and counts as dropped (TBR). The dropped HEALTH are counted, with the onboard time
 *     of the first and the last (REQ-076).
 *   - The report number continues across restarts: it is kept in the file (REQ-071, REQ-077).
 *
 * File layout. A sequence of records, nothing else (Proposal, TBR, awaiting the owner's
 * acceptance):
 *     offset  bytes  field
 *          0      1  tag: 0x52 ('R') a report, 0x53 ('S') the store's state
 *          1      1  n, the body length: a report 15 to 47 (Sec. 5.4), the state 24
 *          2      n  body
 *        2+n      4  CRC-32 (crc32.hpp, Interfaces Sec. 4.0) of bytes 0 to 1+n, little-endian
 *   A report body is the report exactly as it goes to the ground. A state body is six numbers of 4
 *   bytes, little-endian: next report number, deleted below (reports numbered below it are
 *   deleted), HEALTH dropped through (HEALTH reports numbered up to it are dropped; 0: none),
 *   dropped count, onboard time of the first dropped, onboard time of the last dropped. The last
 *   good state record in the file is the one in force.
 *
 * Power cuts (REQ-077). A record is appended with one write at the end of the good records and
 * flushed (fsync) before the next one. A cut can only spoil the record being written: at start,
 * load() reads the records in order, checks each length before the bytes it covers and each CRC,
 * and cuts the file at the first one that fails (logged), so the records before it are kept.
 * A rewrite (compaction: the state and the stored reports only) goes to "<name>.tmp", is
 * flushed, renamed over the file and the directory is flushed, as durable_record.hpp does: a cut
 * leaves the old file or the new one. Damage that is not at the end (not from a power cut) also
 * cuts the file there, losing the records after it (TBR, awaiting the owner's acceptance).
 *
 * Open (TBR): numbers are assumed not to wrap (2^32 reports: 136 years at one a second); after a
 * wrap the order of the store is wrong.
 */
#ifndef FSIR_UPDATER_REPORT_STORE_HPP
#define FSIR_UPDATER_REPORT_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol.hpp"

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

// HEALTH reports dropped from the store since the counters were last taken (REQ-076).
struct DroppedHealth
{
  uint32_t count;          // how many; stops at UINT32_MAX
  uint32_t first_time_s;   // onboard computer clock time of the first dropped, in seconds
  uint32_t last_time_s;    // onboard computer clock time of the last dropped, in seconds
};

class ReportStore
{
public:
  // No store on disk: reports are checked, numbered from 1 and kept in RAM only.
  ReportStore();
  // The store on disk: the file config::kReportStoreName in directory, capacity_bytes of stored
  // report records. Touches no file; load() opens it.
  ReportStore(const char * directory, size_t capacity_bytes);

  // At start: reads the file, cuts a damaged tail, creates the file if there is none. False: no
  // store on disk for this run (logged); reports still go live.
  [[nodiscard]] bool load();

  // Checks a report from the controller (length and kind, Sec. 5.4) and writes into out
  // (room bytes) the report for the ground: the same bytes with the report number and the
  // onboard computer clock time filled in, and stores it. False if the report does not fit
  // Sec. 5.4.
  bool accept(const uint8_t * payload, size_t length, uint32_t onboard_time_s, uint64_t now_ms,
    uint8_t * out, size_t room);

  bool haveHealth() const;
  const HealthReport & lastHealth() const;
  uint64_t lastHealthMs() const;   // when it arrived (clocks::nowMs)
  uint32_t lastNumber() const;     // the last number given; 0 before the first

  bool onDisk() const;             // load() succeeded: reports are stored
  uint32_t nextNumber() const;     // the number the next report gets
  // SEND REPORTS, in this order: deleteBelow(first) deletes the reports the ground has;
  // takeDropped() gives the dropped-HEALTH counters and sets them to zero in RAM; the updater
  // stores EVENT 0x0104; startSend(first, end) writes the zeroed counters and makes nextToSend()
  // give the stored reports numbered first to end - 1, the EVENT reports first.
  void deleteBelow(uint32_t first);
  DroppedHealth takeDropped();
  void startSend(uint32_t first, uint32_t end);
  bool sending() const;
  // The next stored report to send, into out (room bytes, length set). False: none left, or the
  // file could not be read (logged); the sending has then ended.
  [[nodiscard]] bool nextToSend(uint8_t * out, size_t room, size_t & length);
  void markSent();                 // the report nextToSend() gave went to the ground
  void noteSendFailed();           // it did not; after kStoredSendTriesMax in a row, stop

private:
  // Where the sending of a contact has got to.
  struct SendCursor
  {
    bool active;
    bool health_pass;       // false: the EVENT reports (first, REQ-078); true: the HEALTH reports
    uint32_t first;         // send reports numbered first ...
    uint32_t end;           // ... to end - 1
    size_t offset;          // where the search goes on in the file
    uint32_t last_sent;     // number of the last report sent in this pass; 0: none
    uint32_t generation;    // the file rewrite the offset belongs to
    size_t pending_next;    // the offset after the report nextToSend() gave
    uint32_t pending_number;  // its number
    uint32_t failures;      // sends that failed in a row
    uint32_t events_sent;
    uint32_t health_sent;
  };

  // The largest body of a record: an EVENT of 47 bytes (Sec. 5.4); the state has 24.
  static constexpr size_t kBodyMax = report::kEventMaxBytes;
  // The longest path (directory, "/", name, ".tmp", zero). As durable_record.hpp: a fixed buffer,
  // nothing allocated; 256 is far longer than the data directory paths in use.
  static constexpr size_t kPathMaxBytes = 256;

  void noteHealth(const uint8_t * payload, uint64_t now_ms);
  void store(const uint8_t * numbered, size_t length);
  bool live(uint32_t number, uint8_t kind) const;
  void dropOldestHealth(size_t need);
  void countDropped(uint32_t onboard_time_s);
  bool appendRecord(uint8_t tag, const uint8_t * body, size_t body_bytes);
  bool appendState();
  void stateBody(uint8_t * body) const;
  bool compact();
  bool writeCompacted(int out_fd, size_t & written, size_t & stored, int & error);
  bool loadScan(int fd, size_t file_size, size_t & good_end);
  bool countLive();
  void stopSend(const char * why);

  uint32_t next_number_;
  bool have_health_;
  HealthReport health_;
  uint64_t health_ms_;

  bool configured_;             // a directory was given and the paths fit
  bool disk_;                   // load() succeeded: reports are stored
  size_t capacity_;             // config::kReportStoreBytes in the program
  size_t file_bytes_;           // the good records end here; the next one is written here
  size_t live_bytes_;           // stored report records, framing included
  uint32_t deleted_below_;      // reports numbered below it are deleted
  uint32_t health_dropped_through_;   // HEALTH numbered up to it are dropped; 0: none
  DroppedHealth dropped_;       // counters for EVENT 0x0104
  size_t health_scan_from_;     // no stored HEALTH lies before this offset
  uint32_t generation_;         // counts the rewrites of the file
  bool events_over_logged_;     // "events alone fill the store" was logged
  SendCursor send_;
  std::array<char, kPathMaxBytes> directory_;
  std::array<char, kPathMaxBytes> path_;
  std::array<char, kPathMaxBytes> temp_path_;
};

const char * modeName(uint8_t mode);
const char * slotName(uint8_t slot);
const char * eventName(uint16_t code);   // Sec. 5.4 event table, for the log
const char * refusalName(uint8_t code);   // Sec. 4.2 refusal table (0x01 to 0x11) and the bench codes, for the log
void describeEvent(uint16_t code, const uint8_t * detail, size_t detail_length, char * out,
  size_t room);   // the log text of one EVENT, detail decoded
const char * checkResultName(uint8_t result);   // Sec. 5.4 check-result table (events 0x0002, 0x0003); null if not listed
const char * faultKindName(uint8_t kind);   // fault_codes.h FAULT_KIND_* (event 0x0001); null if not listed
const char * faultLocationName(uint8_t where);   // fault_codes.h FAULT_AT_* (event 0x0005); null if not listed

#endif  // FSIR_UPDATER_REPORT_STORE_HPP
