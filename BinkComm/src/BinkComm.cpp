#include "bink/BinkComm.h"

BinkReceiver::BinkReceiver(BinkHandler &handler) : handler(handler) {}

void BinkReceiver::Feed(const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++)
    Feed(data[i]);
}

void BinkReceiver::Feed(uint8_t byte) {
  if (byte & 0x80) { // a header always starts a message
    id = byte & 0x7F;
    state = 1;
  } else if (state == 1) {
    data1 = byte;
    state = 2;
  } else if (state == 2) {
    Dispatch(id, (data1 >> 5) & 3, ((data1 & 0x1F) << 7) | byte);
    id = (id + 1) & 0x7F; // the next message may leave out its header
    state = 1;
  } // state 0: joined mid stream, wait for a header
}

void BinkReceiver::Dispatch(int id, int type, int value) {
  if (type == BINK_STAT)
    return;

  if (type == BINK_VALUE) {
    if (id == BINK_SPECIAL_OCTAVES && value >= 1 && value <= BINK_MAX_OCTAVES) {
      octaves = value;
      handler.OnOctaves(value);
      return;
    }
    if (id >= BINK_SPECIAL_TEMPERATURE_0 && id < BINK_SPECIAL_TEMPERATURE_0 + BINK_TEMPERATURE_COUNT) {
      const int channel = id - BINK_SPECIAL_TEMPERATURE_0;
      temperatures[channel] = (uint16_t)value;
      handler.OnTemperature(channel, value);
      return;
    }
  }

  const int count = octaves;
  if (count > 0 && id == BINK_AMBIENT_ID(count)) {
    if (type == BINK_VALUE)
      handler.OnAmbient(value);
    return;
  }
  // Nothing but keys from here. Without a count any ID in range may be one.
  if (id >= (count > 0 ? BINK_KEY_COUNT(count) : kMaxIds))
    return;

  switch (type) {
  case BINK_VALUE:
    positions[id] = (uint16_t)value;
    handler.OnPosition(id, value);
    break;
  case BINK_NOTEON:
    handler.OnNoteOn(id, value);
    break;
  case BINK_NOTEOFF:
    handler.OnNoteOff(id, value);
    break;
  }
}

int BinkReceiver::Position(int key) const { return key >= 0 && key < kMaxIds ? positions[key] : 0; }

int BinkReceiver::TemperatureValue(int channel) const {
  return channel >= 0 && channel < BINK_TEMPERATURE_COUNT ? temperatures[channel] : 0;
}

float BinkReceiver::Temperature(int channel) const {
  const float volts = TemperatureValue(channel) * 3.3f / 4095.0f;
  return (volts - 0.5f) / 0.01f;
}

bool BinkReceiver::SetOctaves(int count) {
  if (count < 1 || count > BINK_MAX_OCTAVES)
    return false;
  octaves = 0; // the probe keeps asking until the keybed confirms
  SendCommand((uint8_t)(BINK_COMMAND_SETOCTAVES_1 + count - 1));
  RequestOctaves();
  return true;
}
