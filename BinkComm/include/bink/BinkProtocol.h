#ifndef BINK_PROTOCOL_H
#define BINK_PROTOCOL_H
// The Bink keybed's serial protocol. Plain constants, usable from C and C++. docs/host_protocol.md has the full story.
//
// A message is three bytes:
//
//   header   1 i i i i i i i   ID, 0..127
//   data 1   0 t t v v v v v   type, value bits 11..7
//   data 2   0 v v v v v v v   value bits 6..0
//
// The header is left out when the ID is the previous message's ID plus one.
#include <stdint.h>

#define BINK_MAX_OCTAVES 7
#define BINK_TEMPERATURE_COUNT 16

// Keys of a keybed of `octaves` octaves: 12 per octave and the top C. Their IDs start at 0, the lowest C.
#define BINK_KEY_COUNT(octaves) ((octaves) * 12 + 1)
// The ambient field sensor's ID comes right after the keys. It is not a key.
#define BINK_AMBIENT_ID(octaves) BINK_KEY_COUNT(octaves)

// Message types.
enum
{
    BINK_VALUE = 0,   // key position, or an answer on one of the IDs below
    BINK_NOTEON = 1,  // value: strike velocity
    BINK_NOTEOFF = 2, // value: release velocity
    BINK_STAT = 3     // keep-alive and debug
};

// IDs of the answers.
enum
{
    BINK_SPECIAL_OCTAVES = 90,
    BINK_SPECIAL_TEMPERATURE_0 = 100,
    BINK_SPECIAL_TEMPERATURE_1 = 101,
    BINK_SPECIAL_TEMPERATURE_2 = 102,
    BINK_SPECIAL_TEMPERATURE_3 = 103,
    BINK_SPECIAL_TEMPERATURE_4 = 104,
    BINK_SPECIAL_TEMPERATURE_5 = 105,
    BINK_SPECIAL_TEMPERATURE_6 = 106,
    BINK_SPECIAL_TEMPERATURE_7 = 107,
    BINK_SPECIAL_TEMPERATURE_8 = 108,
    BINK_SPECIAL_TEMPERATURE_9 = 109,
    BINK_SPECIAL_TEMPERATURE_10 = 110,
    BINK_SPECIAL_TEMPERATURE_11 = 111,
    BINK_SPECIAL_TEMPERATURE_12 = 112,
    BINK_SPECIAL_TEMPERATURE_13 = 113,
    BINK_SPECIAL_TEMPERATURE_14 = 114,
    BINK_SPECIAL_TEMPERATURE_15 = 115
};

// Commands, one byte each.
enum
{
    BINK_COMMAND_UARTMODE = 'U',
    BINK_COMMAND_USBMODE = 'u',
    BINK_COMMAND_LOCALMIDI = 'M', // reserved
    BINK_COMMAND_MIDI = 'm',      // reserved
    BINK_COMMAND_SPIHOST = 'S',   // reserved
    BINK_COMMAND_SPICLIENT = 's', // reserved

    BINK_COMMAND_SENDTEMPERATURES = 'T',
    BINK_COMMAND_SENDOCTAVES = 'O',
    BINK_COMMAND_SETOCTAVES_1 = '1',
    BINK_COMMAND_SETOCTAVES_2 = '2',
    BINK_COMMAND_SETOCTAVES_3 = '3',
    BINK_COMMAND_SETOCTAVES_4 = '4',
    BINK_COMMAND_SETOCTAVES_5 = '5',
    BINK_COMMAND_SETOCTAVES_6 = '6',
    BINK_COMMAND_SETOCTAVES_7 = '7',
    BINK_COMMAND_SETDATAMODE_COMPENSATED = 'C',
    BINK_COMMAND_SETDATAMODE_RAW = 'R',

    BINK_COMMAND_CAPTUREZEROLEVELS = '_',
    BINK_COMMAND_CAPTUREMEDIUMLEVELS = '-',
    BINK_COMMAND_CAPTUREHIGHLEVELS = '^',

    BINK_COMMAND_SAVEEEPROM = 'E'
};

#endif
