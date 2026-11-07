#pragma once
// The host side of the Bink protocol. It has no dependencies and knows nothing of the link: feed it the bytes that arrive,
// and implement BinkHandler to get the events and to send commands.
//
//   class Keybed : public BinkHandler {
//     void SendByte(uint8_t command) override { serial_write(command); }
//     void OnNoteOn(int key, int velocity) override { ... }
//   };
//
//   Keybed keybed;
//   BinkReceiver bink(keybed);
//   bink.RequestOctaves();
//   while (serial_read(&byte)) bink.Feed(byte);
#include "BinkProtocol.h"

#include <stddef.h>
#include <stdint.h>

// Handlers run inside BinkReceiver::Feed, so in whatever thread or interrupt feeds it. Key 0 is the lowest C.
class BinkHandler {
public:
  virtual ~BinkHandler() {}

  virtual void SendByte(uint8_t command) = 0;

  virtual void OnPosition(int /*key*/, int /*value*/) {}   // 0 at rest, rises as the key goes down and on into aftertouch
  virtual void OnNoteOn(int /*key*/, int /*velocity*/) {}  // velocity: 0..4095
  virtual void OnNoteOff(int /*key*/, int /*velocity*/) {}
  virtual void OnAmbient(int /*value*/) {}             // the ambient field sensor
  virtual void OnOctaves(int /*octaves*/) {}           // every announcement, also when nothing changed
  virtual void OnTemperature(int /*channel*/, int /*value*/) {} // channel 0..15, a 12-bit ADC reading
};

class BinkReceiver {
public:
  explicit BinkReceiver(BinkHandler &handler);

  void Feed(uint8_t byte);
  void Feed(const uint8_t *data, size_t length);

  // The keybed's announced octave count, 0 until it has announced one. Key positions and notes before that are passed on
  // as they come, since it is not known yet which ID is the ambient sensor.
  int Octaves() const { return octaves; }
  int KeyCount() const { return octaves > 0 ? BINK_KEY_COUNT(octaves) : 0; }
  void ForgetOctaves() { octaves = 0; } // for a link that went away

  int Position(int key) const;           // the last position received, 0 for a key that has not moved
  int TemperatureValue(int channel) const; // the last reading received, 0 if none
  float Temperature(int channel) const;  // the same in degrees C, for an MCP9700A

  void SendCommand(uint8_t command) { handler.SendByte(command); }
  void RequestOctaves() { SendCommand(BINK_COMMAND_SENDOCTAVES); }
  void RequestTemperatures() { SendCommand(BINK_COMMAND_SENDTEMPERATURES); }
  // Reconfigures the keybed and asks it to confirm. False for a count out of range.
  bool SetOctaves(int count);

private:
  static const int kMaxIds = BINK_KEY_COUNT(BINK_MAX_OCTAVES) + 1;

  void Dispatch(int id, int type, int value);

  BinkHandler &handler;
  int state = 0; // 0: waiting for a header, 1: expecting data 1, 2: expecting data 2
  int id = 0;
  uint8_t data1 = 0;
  volatile int octaves = 0;
  uint16_t positions[kMaxIds] = {};
  uint16_t temperatures[BINK_TEMPERATURE_COUNT] = {};
};
