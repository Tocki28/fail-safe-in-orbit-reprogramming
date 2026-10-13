#include "command_gate.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "clock.hpp"
#include "log.hpp"

namespace
{

/**
 * @brief Stores a UDP port into a socket address in network byte order.
 *
 * Port in network byte order (most significant byte first), written byte by byte so it does
 * not depend on the processor's byte order.
 *
 * @param[in,out] address socket address whose sin_port is set
 * @param[in]     port  UDP port number, host value
 * @return nothing
 */
void setPort(sockaddr_in & address, uint16_t port)
{
  std::array<uint8_t, 2> big_endian = {static_cast<uint8_t>(port >> 8),
                                       static_cast<uint8_t>(port & 0xFFU)};
  std::memcpy(&address.sin_port, big_endian.data(), big_endian.size());
}

/**
 * @brief Reads the UDP port out of a socket address as a host value.
 *
 * Reads sin_port byte by byte as most significant byte first, so it does not depend on the
 * processor's byte order.
 *
 * @param[in]    address  socket address whose sin_port is read
 * @return UDP port number, host value
 */
uint16_t getPort(const sockaddr_in & address)
{
  std::array<uint8_t, 2> big_endian{};
  std::memcpy(big_endian.data(), &address.sin_port, big_endian.size());
  return static_cast<uint16_t>((static_cast<unsigned>(big_endian[0]) << 8) | big_endian[1]);
}

/**
 * @brief Gives the name of a frame destination for log lines.
 *
 * Names the three destinations of the frame header; any other value reads "unknown".
 *
 * @param[in]    destination  Destination byte of a frame
 * @return text of the name; never null
 */
const char * destinationName(uint8_t destination)
{
  switch (destination) {
    case frame::kDestOnboard:    return "onboard computer";
    case frame::kDestController: return "controller";
    case frame::kDestGround:     return "ground";
    default:                     return "unknown";   // input from the link: a raw Destination byte; any other value is unknown
  }
}

}  // namespace

CommandGate::CommandGate(uint16_t port)
: port_(port), fd_(-1), ground_{}, have_ground_(false), next_sequence_(1), dropped_(0),
  dropped_logged_(0), suppressed_(0), last_drop_log_ms_(0), rx_{}, tx_{}
{
}

/**
 * @brief Closes the UDP socket if it is open.
 *
 * The socket is closed here so that it is released on every exit path.
 * @return nothing (destructor)
 */
CommandGate::~CommandGate()
{
  if (fd_ >= 0) {
    (void)::close(fd_);
  }
}

/**
 * @brief Opens the UDP socket on all interfaces.
 *
 * Opens the UDP socket on all interfaces, non-blocking and close-on-exec. False if that failed;
 * errno tells why. Binds INADDR_ANY on the gate's port; on a bind failure the socket is closed again and errno is kept.
 * @return true: socket open and bound; false: socket() or bind() failed, errno holds the cause
 */
bool CommandGate::open()
{
  // Non-blocking, so a datagram that poll() reported and a checksum dropped cannot make
  // recvfrom() wait; close-on-exec, so no child process ever inherits the ground socket
  // (audit finding #10). On Linux both are set in the one call; elsewhere (the Mac that runs
  // the tests) fcntl does the same after it.
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    return false;
  }
#else
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) {
    return false;
  }
  int flags = ::fcntl(fd_, F_GETFL);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0 ||
      ::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0) {
    int saved = errno;
    (void)::close(fd_);
    fd_ = -1;
    errno = saved;
    return false;
  }
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = 0;   // INADDR_ANY: every interface (0.0.0.0 in any byte order)
  setPort(address, port_);
  if (::bind(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0) {
    int saved = errno;
    (void)::close(fd_);
    fd_ = -1;
    errno = saved;
    return false;
  }
  return true;
}

/**
 * @brief Gives the ground socket's descriptor, for poll().
 *
 * @return the descriptor; -1 while the socket is closed
 */
int CommandGate::fd() const
{
  return fd_;
}

/**
 * @brief Tells whether a ground has sent a good frame yet (reports have somewhere to go).
 *
 * @return true: a ground address is known; false: none yet
 */
bool CommandGate::haveGround() const
{
  return have_ground_;
}

/**
 * @brief Reads one datagram and accepts it only if it is a good command frame.
 *
 * Reads one datagram. True when it is a good command frame for the onboard computer or the
 * controller; command.payload then points into the gate, valid until the next call.
 * Anything else is dropped and logged, without an answer (Interfaces Sec. 4.2).
 * The source address and port of an accepted command become the target of answers and reports.
 *
 * @param[out]   command  decoded frame; payload points into the gate's receive buffer
 * @return true: a good command for the onboard computer or the controller; false: nothing read, or the datagram was dropped
 */
bool CommandGate::receive(frame::Frame & command)
{
  sockaddr_in source{};
  socklen_t source_size = sizeof source;
  ssize_t size = ::recvfrom(fd_, rx_.data(), rx_.size(), 0,
    reinterpret_cast<sockaddr *>(&source), &source_size);
  if (size < 0) {
    if (errno != EAGAIN && errno != EINTR) {
      logging::line("ground link: receive failed: %s", std::strerror(errno));
    }
    return false;
  }
  std::array<char, INET_ADDRSTRLEN> name{};
  (void)inet_ntop(AF_INET, &source.sin_addr, name.data(), static_cast<socklen_t>(name.size()));

  logging::frame("ground link: received", rx_.data(), static_cast<size_t>(size));
  // Damaged, cut short, too long, wrong Sync or Version: dropped without an answer
  // (Interfaces Sec. 4.0 and 4.2; REQ-107).
  if (!frame::decode(rx_.data(), static_cast<size_t>(size), command)) {
    if (dropLogDue()) {
      logging::line("ground link: datagram of %zd bytes from %s:%u is not a good frame; dropped",
        size, name.data(), getPort(source));
    }
    return false;
  }
  if (command.type != frame::kTypeCommand) {
    if (dropLogDue()) {
      logging::line("ground link: frame type 0x%02x from the ground is not a command; dropped",
        command.type);
    }
    return false;
  }
  if (command.destination != frame::kDestOnboard &&
      command.destination != frame::kDestController) {
    if (dropLogDue()) {
      logging::line("ground link: command sequence %u for destination 0x%02x; dropped",
        command.sequence, command.destination);
    }
    return false;
  }

  if (!have_ground_ || ground_.sin_addr.s_addr != source.sin_addr.s_addr ||
      ground_.sin_port != source.sin_port) {
    logging::line("ground link: answers and reports now go to %s:%u", name.data(),
      getPort(source));
  }
  ground_ = source;
  have_ground_ = true;

  logging::line("ground: command 0x%02x sequence %u for the %s (%u payload bytes)",
    (command.length > 0) ? command.payload[0] : 0U, command.sequence,
    destinationName(command.destination), command.length);
  return true;
}

/**
 * @brief Counts a dropped datagram and says whether its log line may be printed now.
 *
 * Anyone on the network can send bad datagrams as fast as the link takes them; one log line each
 * would fill the log of an update in progress (audit finding #8). At most one line per
 * config::kDropLogEveryMs is printed, and it is preceded by a line that says how many were
 * suppressed since the last one.
 *
 * @return true: print the line for this datagram; false: suppressed
 */
bool CommandGate::dropLogDue()
{
  dropped_++;
  uint64_t now = clocks::nowMs();
  if (dropped_logged_ > 0 && now - last_drop_log_ms_ < config::kDropLogEveryMs) {
    suppressed_++;
    return false;
  }
  if (suppressed_ > 0) {
    logging::line("ground link: %u more datagrams dropped since the last such line", suppressed_);
    suppressed_ = 0;
  }
  dropped_logged_++;
  last_drop_log_ms_ = now;
  return true;
}

/**
 * @brief Gives the number of datagrams dropped since start.
 *
 * Counted for every drop, logged or not (audit finding #8).
 *
 * @return count of dropped datagrams
 */
uint32_t CommandGate::droppedCount() const
{
  return dropped_;
}

/**
 * @brief Gives how many of the dropped datagrams got a log line.
 *
 * The rest were suppressed by the rate limit (config::kDropLogEveryMs).
 *
 * @return count of logged drops
 */
uint32_t CommandGate::droppedLogged() const
{
  return dropped_logged_;
}

/**
 * @brief Sends one answer frame to the ground.
 *
 * One answer to the ground (Destination 0x03): to the command with ground sequence number
 * answered.
 *
 * @param[in]    answered  ground sequence number of the command being answered
 * @param[in]    kind  answer kind byte
 * @param[in]    code  answer code byte
 * @return true: sent; false: no ground known yet, frame too large, or send failed
 */
bool CommandGate::answer(uint32_t answered, uint8_t kind, uint8_t code)
{
  std::array<uint8_t, frame::kAnswerBytes> payload{};
  frame::encodeAnswer(frame::Answer{answered, kind, code}, payload.data());
  return sendToGround(frame::kTypeAnswer, payload.data(), static_cast<uint16_t>(payload.size()));
}

/**
 * @brief Sends one report frame to the ground.
 *
 * One report to the ground (Destination 0x03), payload as made by the report store.
 *
 * @param[in]    payload  report payload bytes
 * @param[in]    length  payload length, bytes
 * @return true: sent; false: no ground known yet, payload does not fit a frame, or send failed
 */
bool CommandGate::sendReport(const uint8_t * payload, uint16_t length)
{
  return sendToGround(frame::kTypeReport, payload, length);
}

/**
 * @brief Frames a payload and sends it to the ground address last heard from.
 *
 * Uses the ground link's own sequence number, one higher than the last (Interfaces Sec. 4.0),
 * with Expiry 0 in answers and reports (Interfaces Sec. 4.0). The sequence number advances only
 * when the frame was encoded.
 *
 * @param[in]    type  frame Type byte
 * @param[in]    payload  payload bytes
 * @param[in]    length  payload length, bytes
 * @return true: sent whole; false: no ground known, payload does not fit a frame, or send failed
 */
bool CommandGate::sendToGround(uint8_t type, const uint8_t * payload, uint16_t length)
{
  if (!have_ground_) {
    return false;   // no ground has spoken yet: nowhere to send
  }
  frame::Frame f{};
  f.type = type;
  f.destination = frame::kDestGround;
  f.flags = 0;
  f.length = length;
  f.sequence = next_sequence_;
  f.expiry = 0;   // 0 in answers and reports (Interfaces Sec. 4.0)
  f.payload = payload;
  size_t size = frame::encode(f, tx_.data(), tx_.size());
  if (size == 0) {
    logging::line("ground link: payload of %u bytes does not fit a frame; not sent", length);
    return false;
  }
  next_sequence_ = (next_sequence_ == UINT32_MAX) ? 1U : next_sequence_ + 1U;

  logging::frame("ground link: sent", tx_.data(), size);
  ssize_t sent = ::sendto(fd_, tx_.data(), size, 0,
    reinterpret_cast<const sockaddr *>(&ground_), sizeof ground_);
  if (sent < 0 || static_cast<size_t>(sent) != size) {
    logging::line("ground link: send failed: %s", std::strerror(errno));
    return false;
  }
  return true;
}

ForwardQueue::ForwardQueue()
: items_{}, head_(0), count_(0)
{
}

/**
 * @brief Tells whether the queue holds no command.
 *
 * @return true: empty; false: at least one command waits
 */
bool ForwardQueue::empty() const
{
  return count_ == 0;
}

/**
 * @brief Tells whether the queue is full (config::kForwardQueueSize commands).
 *
 * @return true: full; false: there is room
 */
bool ForwardQueue::full() const
{
  return count_ == items_.size();
}

/**
 * @brief Adds a ground command for the controller at the end of the queue.
 *
 * Kept in arrival order for data link A, which carries one command at a time [Interfaces,
 * Sec. 4.2]. The armed mark travels with the command so that, when its turn comes, the updater
 * sends the ARM/command pair [Interfaces, Sec. 5.2].
 *
 * @param[in]     command  the ground command; its payload is copied
 * @param[in]     armed    true: a dangerous command whose ARM was checked and used up
 * @param[in]     arm_at_ms  when that ARM arrived, ms; kept so that its age is checked again just
 *                         before the ARM goes on to the controller (REQ-030); ignored if !armed
 * @return true: queued; false: the queue is full or the payload is too long
 */
bool ForwardQueue::push(const frame::Frame & command, bool armed, uint64_t arm_at_ms)
{
  if (full() || command.length > frame::kPayloadMax) {
    return false;
  }
  ForwardedCommand & item = items_[(head_ + count_) % items_.size()];
  item.ground_sequence = command.sequence;
  item.expiry = command.expiry;
  item.length = command.length;
  item.armed = armed;
  item.arm_at_ms = arm_at_ms;
  std::memcpy(item.payload.data(), command.payload, command.length);
  count_++;
  return true;
}

/**
 * @brief Gives the oldest command in the queue.
 *
 * Only valid when the queue is not empty; callers check empty() first.
 *
 * @return the oldest command, still in the queue
 */
const ForwardedCommand & ForwardQueue::front() const
{
  return items_[head_];
}

/**
 * @brief Removes the oldest command from the queue.
 *
 * Does nothing when the queue is empty.
 * @return nothing
 */
void ForwardQueue::pop()
{
  if (count_ > 0) {
    head_ = (head_ + 1) % items_.size();
    count_--;
  }
}

ForwardTable::ForwardTable()
: entries_{}, next_(0)
{
}

/**
 * @brief Records which ground sequence number a data-link sequence number carried.
 *
 * Replaces the oldest entry when full.
 *
 * @param[in]    link_sequence  sequence number used on data link A
 * @param[in]    ground_sequence  sequence number the ground used for the same command
 * @return nothing
 */
void ForwardTable::add(uint32_t link_sequence, uint32_t ground_sequence)
{
  entries_[next_] = Entry{true, link_sequence, ground_sequence};
  next_ = (next_ + 1) % entries_.size();
}

/**
 * @brief Looks up the ground sequence number of a command sent on data link A.
 *
 * @param[in]     link_sequence    the sequence number the command carried on data link A
 * @param[out]    ground_sequence  the ground's sequence number; written only when true is returned
 * @return true: found; false: not in the table (never added, or replaced since)
 */
bool ForwardTable::find(uint32_t link_sequence, uint32_t & ground_sequence) const
{
  for (const Entry & entry : entries_) {
    if (entry.used && entry.link_sequence == link_sequence) {
      ground_sequence = entry.ground_sequence;
      return true;
    }
  }
  return false;
}

/**
 * @brief Forgets the entry for a data-link sequence number.
 *
 * Does nothing when no entry has that number.
 *
 * @param[in]    link_sequence  sequence number used on data link A
 * @return nothing
 */
void ForwardTable::remove(uint32_t link_sequence)
{
  for (Entry & entry : entries_) {
    if (entry.used && entry.link_sequence == link_sequence) {
      entry.used = false;
    }
  }
}

UsedSequences::UsedSequences()
: entries_{}, next_(0)
{
}

/**
 * @brief Tells whether a ground sequence number is among the last config::kUsedSequenceCount used.
 *
 * @param[in]     sequence  the ground sequence number of a command
 * @return true: used before (a repeat, REFUSED 0x04, REQ-065); false: not in the ring
 */
bool UsedSequences::contains(uint32_t sequence) const
{
  for (const Entry & entry : entries_) {
    if (entry.used && entry.sequence == sequence) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Remembers a ground sequence number as used.
 *
 * Replaces the oldest entry when the ring of config::kUsedSequenceCount is full (REQ-065).
 *
 * @param[in]     sequence  the ground sequence number of an accepted ARM or dangerous command
 * @return nothing
 */
void UsedSequences::add(uint32_t sequence)
{
  entries_[next_] = Entry{true, sequence};
  next_ = (next_ + 1) % entries_.size();
}
