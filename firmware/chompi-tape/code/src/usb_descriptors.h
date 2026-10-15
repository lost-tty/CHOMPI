#pragma once

enum
{
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING_RX, // host -> CHOMPI
    ITF_NUM_AUDIO_STREAMING_TX, // CHOMPI -> host
    ITF_NUM_MIDI,
    ITF_NUM_MIDI_STREAMING,
    ITF_NUM_MSC, // the card as a drive
    ITF_NUM_CDC, // the log
    ITF_NUM_CDC_DATA,
    ITF_NUM_TOTAL
};

// UAC2 entity IDs, shared by the descriptors and the audio control requests
#define UAC2_ENTITY_CLOCK              0x04
#define UAC2_ENTITY_RX_INPUT_TERMINAL  0x01 // host -> CHOMPI
#define UAC2_ENTITY_RX_OUTPUT_TERMINAL 0x03
#define UAC2_ENTITY_TX_INPUT_TERMINAL  0x11 // CHOMPI -> host
#define UAC2_ENTITY_TX_OUTPUT_TERMINAL 0x13
