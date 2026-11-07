#include "DevKitKeybed.h"
#include "DevKitHal.h"

static const int kFirstNote = 24;   // MIDI note of key 0
static const int kLostNoteOff = 30; // a held key this high up has lost its note off: the keybed sends one below 100

DevKitKeybed TheKeybed;

void DevKitKeybed::Init() {
  hal_bink_init();
  Bink.SendCommand(BINK_COMMAND_UARTMODE);
}

void DevKitKeybed::Drain() {
  uint8_t byte;
  while (hal_bink_read(byte))
    Bink.Feed(byte);
}

void DevKitKeybed::ReleaseAll() {
  for (int key = 0; key < BINK_KEY_COUNT(BINK_MAX_OCTAVES) + 1; key++)
    if (held[key]) {
      held[key] = false;
      SendAudioEvent(AUDIOEVENT_NOTEOFF, (uint8_t)(key + kFirstNote), 0);
    }
}

void DevKitKeybed::SendByte(uint8_t command) { hal_bink_send(command); }

// Notes come from the keybed's own note messages, which carry the velocity. Positions only shape a note that is sounding.
void DevKitKeybed::OnPosition(int key, int value) {
  if (!held[key])
    return;
  const uint8_t note = (uint8_t)(key + kFirstNote);
  if (value < kLostNoteOff) {
    held[key] = false;
    SendAudioEvent(AUDIOEVENT_NOTEOFF, note, 0);
  } else {
    SendAudioEvent(AUDIOEVENT_VALUE, note, (uint16_t)value);
  }
}

void DevKitKeybed::OnNoteOn(int key, int velocity) {
  held[key] = true;
  SendAudioEvent(AUDIOEVENT_NOTEONVELOCITY, (uint8_t)(key + kFirstNote), (uint16_t)velocity);
}

void DevKitKeybed::OnNoteOff(int key, int velocity) {
  held[key] = false;
  SendAudioEvent(AUDIOEVENT_NOTEOFFVELOCITY, (uint8_t)(key + kFirstNote), (uint16_t)velocity);
}

void DevKitKeybed::OnOctaves(int) { OctavesRxUs = hal_time_us(); }
