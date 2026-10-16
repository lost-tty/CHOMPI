#pragma once

#include <cstddef>
#include <cstdint>
#include "tusb_config.h"

namespace chompi
{
    // The external USB port on TinyUSB: UAC2 audio (48 kHz / 24 bit, host -> CHOMPI stereo, CHOMPI -> host the mix
    // and the input) and MIDI. TinyUSB's task runs from TIM7 at priority 2: the USB interrupt (0) has to preempt
    // it, and the main loop and SD work can't delay MIDI. Receive() and Send() run in the audio callback.
    namespace usb
    {
        static constexpr size_t kRxChannels = CHOMPI_USB_CHANNELS_RX;
        static constexpr size_t kTxChannels = CHOMPI_USB_CHANNELS_TX;
        static constexpr size_t kMaxBlock = 48; // Receive() holds one

        void Init();

        // configured and not suspended; without VBUS sensing an unplug is a suspend
        bool Connected();

        // the host plays to CHOMPI
        bool RxStreaming();

        // host audio for this block; fades in when the stream starts, out when it stops or runs dry
        const float *const *Receive(size_t size);

        // kTxChannels to the host, null ones as silence; dropped while it doesn't record
        void Send(const float *const *to_host, size_t size);

        // the card as a USB drive: the medium is in while on, and CHOMPI leaves the card to the host meanwhile
        void CardMode(bool on);
        bool InCardMode();
        // the host ejected it, once
        bool TakeCardEjected();
        // the card task, while the card is free: the host's transfers
        void CardTask();

        // printf to the CDC port, from any context; the USB task sends it. Lines are dropped while it is full.
        void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
        // main's loop calls it: while it stops, the USB task logs where the CPU is
        void MainAlive();
    }

    // libDaisy's MidiHandler transport on TinyUSB: Tx queues from any context, Process() in usb's task sends and receives
    class MidiTinyUsbTransport
    {
      public:
        typedef void (*MidiRxParseCallback)(uint8_t *data, size_t size, void *context);

        struct Config
        {
        };

        void Init(Config config) {}
        void Reset();
        void StartRx(MidiRxParseCallback callback, void *context);
        bool RxActive() { return true; }
        void FlushRx() {}
        bool Tx(uint8_t *buffer, size_t size);

        static void Process();
    };
}
