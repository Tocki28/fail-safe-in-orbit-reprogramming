#include "serial_port.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace
{

// Longest wait for room in the device's send buffer. A frame is at most 1044 bytes and Linux
// takes it whole into the tty buffer (4 KB), so this wait is only reached when the device has
// stopped; 1044 bytes need 90.6 ms on the line.
constexpr int kSendWaitMs = 500;
constexpr int kMaxSendAttempts = 8;

}  // namespace

SerialPort::SerialPort(const char * path)
: path_(path), fd_(-1)
{
}

SerialPort::~SerialPort()
{
  close();
}

bool SerialPort::open()
{
  close();
  // O_NONBLOCK: reads return at once with what has arrived; the main loop waits in poll().
  // O_NOCTTY: the device does not become this program's controlling terminal.
  fd_ = ::open(path_, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    return false;
  }
  if (!configureRaw()) {
    int saved = errno;
    close();
    errno = saved;
    return false;
  }
  return true;
}

void SerialPort::close()
{
  if (fd_ >= 0) {
    (void)::close(fd_);   // nothing more can be done with a device that fails to close
    fd_ = -1;
  }
}

bool SerialPort::isOpen() const
{
  return fd_ >= 0;
}

const char * SerialPort::path() const
{
  return path_;
}

int SerialPort::pollFd() const
{
  return fd_;
}

bool SerialPort::configureRaw()
{
  termios settings{};
  if (tcgetattr(fd_, &settings) != 0) {
    return false;
  }
  // Raw: no echo, no line editing, no translation of CR or LF, 8 data bits. Without it the
  // terminal layer holds bytes until a newline and changes some of them.
  cfmakeraw(&settings);
  // No parity, 1 stop bit, no hardware flow control (the ST-LINK port has no RTS/CTS wired to
  // the controller); CLOCAL: ignore modem lines; CREAD: receive.
  settings.c_cflag &= ~static_cast<tcflag_t>(PARENB | CSTOPB | CRTSCTS);
  settings.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
  // VMIN 1, VTIME 0 with O_NONBLOCK: read() returns at once with whatever has arrived, or -1
  // with EAGAIN when nothing has, so that 0 means only "end of file" (the device went away).
  // With VMIN 0 and VTIME 0, read() returns 0 when no data is available [termios(3), "MIN == 0,
  // TIME == 0 (polling read)"], and the two cases could not be told apart.
  settings.c_cc[VMIN] = 1;
  settings.c_cc[VTIME] = 0;
  if (cfsetispeed(&settings, B115200) != 0 || cfsetospeed(&settings, B115200) != 0) {
    return false;
  }
  if (tcsetattr(fd_, TCSANOW, &settings) != 0) {
    return false;
  }
  // Drop anything received before now: a part frame would only be dropped later.
  return tcflush(fd_, TCIOFLUSH) == 0;
}

bool SerialPort::sendBytes(const uint8_t * data, size_t size)
{
  if (fd_ < 0) {
    return false;
  }
  size_t sent = 0;
  for (int attempt = 0; attempt < kMaxSendAttempts && sent < size; attempt++) {
    ssize_t written = ::write(fd_, data + sent, size - sent);
    if (written > 0) {
      sent += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno != EAGAIN && errno != EINTR) {
      return false;
    }
    // The send buffer is full: wait for room, then try again.
    pollfd watch{};
    watch.fd = fd_;
    watch.events = POLLOUT;
    (void)poll(&watch, 1, kSendWaitMs);
  }
  return sent == size;
}

long SerialPort::receiveBytes(uint8_t * buffer, size_t capacity)
{
  if (fd_ < 0) {
    return -1;
  }
  ssize_t count = ::read(fd_, buffer, capacity);
  if (count > 0) {
    return static_cast<long>(count);
  }
  if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
    return 0;
  }
  // 0 (end of file) or another error: the device went away, e.g. its USB cable was pulled.
  return -1;
}
