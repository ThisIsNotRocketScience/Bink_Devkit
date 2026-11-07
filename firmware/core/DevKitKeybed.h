#pragma once
// The keybed as the DevKit sees it: BinkComm on the HAL's link, turning the keybed's events into audio events.
#include "bink/BinkComm.h"
#include <stdint.h>

enum AudioEventType {
  AUDIOEVENT_NOTEOFF = 0,
  AUDIOEVENT_NOTEON = 1,
  AUDIOEVENT_VALUE = 2,
  AUDIOEVENT_NOTEONVELOCITY = 3,
  AUDIOEVENT_NOTEOFFVELOCITY = 4
};

void SendAudioEvent(uint8_t type, uint8_t note, uint16_t value);

class DevKitKeybed : public BinkHandler {
public:
  DevKitKeybed() : Bink(*this) {}

  void Init();  // opens the link and asks for the UART stream
  void Drain(); // decodes what has arrived
  void ReleaseAll(); // for a link that went away: ends the notes of keys that are down

  BinkReceiver Bink;
  // hal_time_us() of the last octave announcement: the keybed repeats it once a second in UART mode, so a stale one means
  // the link is gone. Written by the core that drains, read by the UI core.
  volatile uint32_t OctavesRxUs = 0;

private:
  void SendByte(uint8_t command) override;
  void OnPosition(int key, int value) override;
  void OnNoteOn(int key, int velocity) override;
  void OnNoteOff(int key, int velocity) override;
  void OnOctaves(int octaves) override;

  bool held[BINK_KEY_COUNT(BINK_MAX_OCTAVES) + 1] = {};
};

extern DevKitKeybed TheKeybed;
