#include "data_link.hpp"

#include "log.hpp"

/**
 * @brief Builds a data link A over a transport.
 *
 * Sequence numbers start at 1 at every start; no command is outstanding and both counters are 0
 * (data_link.hpp).
 *
 * @param[in,out] transport  channel to the controller; held by reference, it must outlive this object
 * @return nothing (constructor)
 */
DataLink::DataLink(Transport & transport)
: transport_(transport), parser_(), rx_{}, tx_{}, next_sequence_(1), busy_(false),
  outstanding_(0), deadline_ms_(0), sent_(0), received_(0)
{
}

/**
 * @brief Reads what has arrived and passes each complete frame to handler.
 *
 * listen_ms is the time the parser measures the 10 ms gap with (main.cpp). Every byte of one read
 * gets the same time (see the comment in the body). Each frame that passes the parser's checks
 * counts in receivedCount().
 *
 * @param[in,out] handler  receives every complete frame, one call per frame
 * @param[in] listen_ms  time given to the parser for every byte of this read, ms
 * @return true: the read succeeded (possibly with no bytes); false: the channel failed
 */
bool DataLink::receive(ControllerFrameHandler & handler, uint64_t listen_ms)
{
  long count = transport_.receiveBytes(rx_.data(), rx_.size());
  if (count < 0) {
    return false;
  }
  // Every byte of one read gets the same time: they arrived while the loop was not listening
  // (main.cpp), so no gap can be measured inside them.
  for (long i = 0; i < count; i++) {
    frame::Frame f{};
    if (parser_.feed(rx_[static_cast<size_t>(i)], listen_ms, f)) {
      received_++;
      handler.onControllerFrame(f);
    }
  }
  return true;
}

/**
 * @brief Forgets a part frame, e.g. after the device was opened again.
 * @return nothing
 */
void DataLink::resetParser()
{
  parser_.reset();
}

/**
 * @brief Sends one command to the controller (Destination 0x02) with the next sequence number and starts its timeout.
 *
 * Only when !busy(). False if the channel failed; nothing is then outstanding. A call while a
 * command is outstanding, or with a payload that does not fit a frame, is refused and logged.
 * The flags byte is 0 (bit 0 is only for a released scheduled command, Interfaces Sec. 5.3).
 *
 * @param[in] payload  command payload; length bytes, read during the call only
 * @param[in] length  payload size, bytes
 * @param[in] expiry  Expiry field of the frame, seconds of ground time (Interfaces Sec. 4.0)
 * @param[in] timeout_ms  time to wait for the last answer, ms
 * @param[in] now_ms  current time, ms; the timeout runs from it
 * @param[out] sequence  sequence number the command carries; set once the frame is encoded
 * @return true: the command was sent and is now outstanding; false: not sent (busy, payload too long, or channel failure)
 */
bool DataLink::sendCommand(const uint8_t * payload, uint16_t length, uint32_t expiry,
  uint32_t timeout_ms, uint64_t now_ms, uint32_t & sequence)
{
  if (busy_) {
    logging::line("data link A: a command is still outstanding; not sent (program error)");
    return false;
  }
  frame::Frame f{};
  f.type = frame::kTypeCommand;
  f.destination = frame::kDestController;
  f.flags = 0;   // bit 0 is only for a released scheduled command (Interfaces Sec. 5.3)
  f.length = length;
  f.sequence = next_sequence_;
  f.expiry = expiry;
  f.payload = payload;
  size_t size = frame::encode(f, tx_.data(), tx_.size());
  if (size == 0) {
    logging::line("data link A: payload of %u bytes does not fit a frame; not sent", length);
    return false;
  }

  sequence = next_sequence_;
  // Numbers run from 1 to 2^32 - 1 (Interfaces Sec. 4.0); 0 is never used.
  next_sequence_ = (next_sequence_ == UINT32_MAX) ? 1U : next_sequence_ + 1U;

  logging::frame("data link A: sent", tx_.data(), size);
  if (!transport_.sendBytes(tx_.data(), size)) {
    logging::line("data link A: sending sequence %u failed", sequence);
    return false;
  }
  sent_++;
  busy_ = true;
  outstanding_ = sequence;
  deadline_ms_ = now_ms + timeout_ms;
  return true;
}

/**
 * @brief Tells the sequence number the next command sent will carry.
 *
 * An ARM names its dangerous command by the sequence number that command carries on this link
 * [Interfaces, Sec. 5.2]; the ARM is sent first, so the updater needs the number before the ARM
 * goes out.
 *
 * @return the next sequence number, 1 to 2^32 - 1
 */
uint32_t DataLink::nextSequence() const
{
  return next_sequence_;
}

/**
 * @brief Tells whether a command is outstanding.
 *
 * A command is outstanding: no other may be sent.
 * @return true: a command is outstanding; false: the link is free
 */
bool DataLink::busy() const
{
  return busy_;
}

/**
 * @brief Tells the sequence number of the command sent last.
 *
 * Valid as the outstanding command while busy() is true; 0 before the first command is sent.
 * @return sequence number of the command sent last
 */
uint32_t DataLink::outstanding() const
{
  return outstanding_;
}

/**
 * @brief Marks the outstanding command as answered.
 *
 * The outstanding command got its last answer.
 * @return nothing
 */
void DataLink::release()
{
  busy_ = false;
}

/**
 * @brief Tells whether the outstanding command's timeout has passed.
 *
 * True once when the outstanding command's timeout has passed; it is then released.
 *
 * @param[in] now_ms  current time, ms
 * @return true: a command was outstanding and its deadline has passed, it is now released; false: otherwise
 */
bool DataLink::timedOut(uint64_t now_ms)
{
  if (!busy_ || now_ms < deadline_ms_) {
    return false;
  }
  busy_ = false;
  return true;
}

/**
 * @brief Tells how many commands were sent.
 *
 * For the log: frames sent and received.
 * @return number of commands sent on the channel since start
 */
uint32_t DataLink::sentCount() const
{
  return sent_;
}

/**
 * @brief Tells how many frames were received.
 *
 * For the log: frames sent and received.
 * @return number of frames from the controller that passed the parser since start
 */
uint32_t DataLink::receivedCount() const
{
  return received_;
}
