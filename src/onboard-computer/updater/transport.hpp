/*
 * transport.hpp — any channel that carries bytes to the controller and back.
 *
 * The data-link code uses only this class, so the channel under it can change (the serial
 * device on the bench, a fake in the unit tests) without changing that code.
 * Virtual functions use a hidden table of function pointers; accepted, as in the owner's link
 * test: two targets (SerialPort, the unit tests' fake), never swapped once created.
 */
#ifndef FSIR_UPDATER_TRANSPORT_HPP
#define FSIR_UPDATER_TRANSPORT_HPP

#include <cstddef>
#include <cstdint>

class Transport
{
public:
  Transport() = default;
  virtual ~Transport() = default;

  // A channel cannot be copied: two objects would own the same channel.
  Transport(const Transport &) = delete;
  Transport & operator=(const Transport &) = delete;

  // Sends all size bytes. False if the channel is closed or failed.
  virtual bool sendBytes(const uint8_t * data, size_t size) = 0;

  // Copies what has arrived into buffer, without waiting. Returns the count (0: nothing yet),
  // or -1 if the channel failed or is closed.
  virtual long receiveBytes(uint8_t * buffer, size_t capacity) = 0;

  // The file descriptor for poll(), or -1 while the channel is closed.
  virtual int pollFd() const = 0;
};

#endif  // FSIR_UPDATER_TRANSPORT_HPP
