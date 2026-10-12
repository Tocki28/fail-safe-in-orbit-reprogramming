/*
 * serial_port.hpp — data link A on the bench: a serial device (the controller board's ST-LINK
 * virtual COM port, /dev/ttyACM0 on the onboard computer), 115200 baud, 8 data bits, no
 * parity, 1 stop bit (Interfaces Sec. 6.2), raw: no echo, no line editing, no character
 * translation, no flow control.
 *
 * Data link A's own code. REQ-099: the two data links share no software, so data link B (CAN,
 * not on this bench) gets its own driver.
 */
#ifndef FSIR_UPDATER_SERIAL_PORT_HPP
#define FSIR_UPDATER_SERIAL_PORT_HPP

#include "transport.hpp"

class SerialPort final : public Transport
{
public:
  // Does not open the device: call open().
  explicit SerialPort(const char * path);

  // Closes the device.
  ~SerialPort() override;

  // Opens the device and sets it to raw 115200 8N1. False if that failed; errno tells why.
  bool open();

  void close();

  bool isOpen() const;

  const char * path() const;

  // see Transport
  bool sendBytes(const uint8_t * data, size_t size) override;
  long receiveBytes(uint8_t * buffer, size_t capacity) override;
  int pollFd() const override;

private:
  bool configureRaw();

  const char * path_;
  int fd_;   // -1 when closed
};

#endif  // FSIR_UPDATER_SERIAL_PORT_HPP
