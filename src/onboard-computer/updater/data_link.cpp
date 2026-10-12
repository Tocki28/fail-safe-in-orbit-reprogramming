#include "data_link.hpp"

#include "log.hpp"

DataLink::DataLink(Transport & transport)
: transport_(transport), parser_(), rx_{}, tx_{}, next_sequence_(1), busy_(false),
  outstanding_(0), deadline_ms_(0), sent_(0), received_(0)
{
}

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

void DataLink::resetParser()
{
  parser_.reset();
}

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

bool DataLink::busy() const
{
  return busy_;
}

uint32_t DataLink::outstanding() const
{
  return outstanding_;
}

void DataLink::release()
{
  busy_ = false;
}

bool DataLink::timedOut(uint64_t now_ms)
{
  if (!busy_ || now_ms < deadline_ms_) {
    return false;
  }
  busy_ = false;
  return true;
}

uint32_t DataLink::sentCount() const
{
  return sent_;
}

uint32_t DataLink::receivedCount() const
{
  return received_;
}
