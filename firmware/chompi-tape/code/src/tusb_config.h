#pragma once

// TinyUSB on the external port: the OTG_HS core's full speed PHY, rhport 1

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU               OPT_MCU_STM32H7
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0

#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED
#define CHOMPI_USB_RHPORT          1

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_AUDIO              1
#define CFG_TUD_MIDI               1
#define CFG_TUD_CDC                1 // the log
#define CFG_TUD_CDC_RX_BUFSIZE     64
#define CFG_TUD_CDC_TX_BUFSIZE     4096
#define CFG_TUD_CDC_EP_BUFSIZE     64
#define CFG_TUD_MSC                1
#define CFG_TUD_HID                0
#define CFG_TUD_VENDOR             0

#define CHOMPI_USB_SAMPLE_RATE     48000
#define CHOMPI_USB_BYTES_PER_SAMPLE 3 // 24 bit packed
#define CHOMPI_USB_BITS_PER_SAMPLE 24
#define CHOMPI_USB_CHANNELS_RX     2 // host -> CHOMPI
#define CHOMPI_USB_CHANNELS_TX     4 // CHOMPI -> host: main mix L/R, input L/R

#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, CHOMPI_USB_SAMPLE_RATE, CHOMPI_USB_BYTES_PER_SAMPLE, CHOMPI_USB_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX \
    TUD_AUDIO_EP_SIZE(false, CHOMPI_USB_SAMPLE_RATE, CHOMPI_USB_BYTES_PER_SAMPLE, CHOMPI_USB_CHANNELS_TX)

// feedback and flow control hold the FIFOs half full: 4 packets, ~2 ms each way
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (4 * CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ  (4 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)

#define CFG_TUD_AUDIO_ENABLE_EP_OUT       1
#define CFG_TUD_AUDIO_ENABLE_EP_IN        1
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP  1
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL  1
// feedback as the spec's 3 byte 10.14 (macOS), not TinyUSB's 4 byte 16.16 (Windows): the CHOMPI change in audio_device.c
#define CFG_TUD_AUDIO20_FS_FEEDBACK_10_14 1

#define CFG_TUD_MIDI_RX_BUFSIZE    64
#define CFG_TUD_MIDI_TX_BUFSIZE    64

#define CFG_TUD_MSC_EP_BUFSIZE     4096 // sectors a transfer

#ifdef __cplusplus
}
#endif
