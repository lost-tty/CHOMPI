#include "UsbDevice.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include "stm32h7xx_hal.h"
#include "daisy_core.h"
#include "util/scopedirqblocker.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "SdDma.h"

namespace chompi
{
    namespace usb
    {
        static constexpr size_t kBytes = CHOMPI_USB_BYTES_PER_SAMPLE;
        static constexpr size_t kRxFrameBytes = kRxChannels * kBytes;
        static constexpr size_t kTxFrameBytes = kTxChannels * kBytes;
        static constexpr float kFullScale = 8388608.f;
        static constexpr int32_t kMax24 = 8388607;
        static constexpr float kFadeInStep = 1000.f / CHOMPI_USB_SAMPLE_RATE; // 1 ms
        static constexpr uint16_t kRxPrime = CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ / 2; // the feedback holds the RX FIFO here

        // what the host asked for, posted by TinyUSB's callbacks in the task
        struct Request
        {
            volatile bool on;
            volatile uint32_t count; // bumped on each change: a quick off and on still restarts
        };
        static Request req_rx = {}, req_tx = {};

        static void Post(Request &req, bool on)
        {
            req.on = on;
            req.count = req.count + 1;
        }

        static void PostStop()
        {
            Post(req_rx, false);
            Post(req_tx, false);
        }

        // the alt settings, posted again on resume
        static bool alt_rx = false, alt_tx = false;

        static bool connected = false;

        // the audio callback's
        static uint32_t seen_rx = 0, seen_tx = 0;
        static bool rx_on = false, tx_on = false;
        static bool rx_primed = false; // the RX FIFO is where the feedback holds it
        static float rx_gain = 0.f;
        static float rx_last[kRxChannels];
        static float rx_buf[kRxChannels][kMaxBlock];
        static const float *rx_ptr[kRxChannels];

        // TinyUSB clears a FIFO in tud_task() before its callback: changes are taken at block boundaries
        static void PickUpRequests()
        {
            if(req_rx.count != seen_rx)
            {
                seen_rx = req_rx.count;
                rx_on = req_rx.on;
                rx_primed = false;
            }
            if(req_tx.count != seen_tx)
            {
                seen_tx = req_tx.count;
                tx_on = req_tx.on;
            }
        }

        void Init()
        {
            for(size_t c = 0; c < kRxChannels; c++)
                rx_ptr[c] = rx_buf[c];

            // PB14/PB15: the OTG_HS core's full speed PHY
            __HAL_RCC_GPIOB_CLK_ENABLE();
            GPIO_InitTypeDef gpio = {};
            gpio.Pin = GPIO_PIN_14 | GPIO_PIN_15;
            gpio.Mode = GPIO_MODE_AF_PP;
            gpio.Pull = GPIO_NOPULL;
            gpio.Speed = GPIO_SPEED_FREQ_HIGH;
            gpio.Alternate = GPIO_AF12_OTG2_FS;
            HAL_GPIO_Init(GPIOB, &gpio);

            __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
            HAL_PWREx_EnableUSBVoltageDetector();

            // USB runs on HSI48: trim it to the host's start of frame
            __HAL_RCC_CRS_CLK_ENABLE();
            RCC_CRSInitTypeDef crs = {};
            crs.Prescaler = RCC_CRS_SYNC_DIV1;
            crs.Source = RCC_CRS_SYNC_SOURCE_USB1;
            crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
            crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
            crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
            crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
            HAL_RCCEx_CRSConfig(&crs);

            // no VBUS pin: the D+/D- switch decides when we're connected
            tud_configure_dwc2_t cfg = CFG_TUD_CONFIGURE_DWC2_DEFAULT;
            cfg.vbus_sensing = false;
            tud_configure(CHOMPI_USB_RHPORT, TUD_CFGID_DWC2, &cfg);

            // the audio DMA's priority: the two never preempt each other
            HAL_NVIC_SetPriority(OTG_HS_IRQn, 0, 0);

            tusb_rhport_init_t dev_init = {};
            dev_init.role = TUSB_ROLE_DEVICE;
            dev_init.speed = TUSB_SPEED_FULL;
            tusb_init(CHOMPI_USB_RHPORT, &dev_init);
            tud_disconnect(); // no D+ pull-up until Task() runs and can answer requests

            // the task timer: TIM7 at 4 kHz, below the SD card (1), above libDaisy's timers
            __HAL_RCC_TIM7_CLK_ENABLE();
            uint32_t clk = HAL_RCC_GetPCLK1Freq();
            if((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != RCC_D2CFGR_D2PPRE1_DIV1)
                clk *= 2; // APB timers run at twice a divided bus
            TIM7->PSC = clk / 1000000 - 1; // 1 MHz
            TIM7->ARR = 250 - 1; // 4 kHz
            TIM7->EGR = TIM_EGR_UG;
            TIM7->SR = 0;
            TIM7->DIER = TIM_DIER_UIE;
            HAL_NVIC_SetPriority(TIM7_IRQn, 2, 0);
            SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
            HAL_NVIC_EnableIRQ(TIM7_IRQn);
            TIM7->CR1 = TIM_CR1_CEN;
        }

        static void Task()
        {
            if(!connected)
            {
                connected = true;
                tud_connect();
            }
            tud_task();
            tud_cdc_write_flush();
            MidiTinyUsbTransport::Process();
        }

        volatile uint32_t main_alive = 0;
        void MainAlive() { main_alive++; }

        bool Connected() { return tud_ready(); }

        void Log(const char *fmt, ...)
        {
            char line[160];
            const int n0 = snprintf(line, sizeof(line), "%8lu ", (unsigned long)HAL_GetTick());
            va_list args;
            va_start(args, fmt);
            int n = vsnprintf(line + n0, sizeof(line) - n0 - 2, fmt, args);
            va_end(args);
            n = n0 + (n < 0 ? 0 : n > int(sizeof(line)) - n0 - 3 ? int(sizeof(line)) - n0 - 3 : n);
            line[n++] = '\r';
            line[n++] = '\n';
            daisy::ScopedIrqBlocker block; // any task and interrupt may log
            if(tud_cdc_write_available() >= uint32_t(n))
                tud_cdc_write(line, n);
        }

        bool RxStreaming() { return rx_on; }

        static inline float Get24(const uint8_t *p) { return s242f(p[0] | p[1] << 8 | p[2] << 16); }

        // f2s24 clamps a 16 bit LSB short of full scale, this doesn't
        static inline void Put24(uint8_t *p, float x)
        {
            int32_t v = (int32_t)(x * kFullScale);
            v = v > kMax24 ? kMax24 : v < -kMax24 - 1 ? -kMax24 - 1 : v;
            p[0] = v;
            p[1] = v >> 8;
            p[2] = v >> 16;
        }

        const float *const *Receive(size_t size)
        {
            PickUpRequests();
            const uint16_t need = size * kRxFrameBytes;
            const uint16_t have = rx_on ? tud_audio_available() : 0;

            // start once the FIFO is where the feedback holds it, and wait for that again after running dry
            if(!rx_primed && have >= kRxPrime)
            {
                rx_primed = true;
                rx_gain = 0.f;
            }
            else if(have < need)
                rx_primed = false;

            if(!rx_primed)
            {
                // stopped or ran dry: down to silence over this block
                const float step = 1.f / size;
                for(size_t c = 0; c < kRxChannels; c++)
                {
                    for(size_t i = 0; i < size; i++)
                        rx_buf[c][i] = rx_last[c] * (1.f - (i + 1) * step);
                    rx_last[c] = 0.f;
                }
                return rx_ptr;
            }

            uint8_t buf[kMaxBlock * kRxFrameBytes];
            tud_audio_read(buf, need);

            for(size_t c = 0; c < kRxChannels; c++)
            {
                const uint8_t *p = buf + c * kBytes;
                float gain = rx_gain;
                for(size_t i = 0; i < size; i++, p += kRxFrameBytes)
                {
                    gain = fminf(gain + kFadeInStep, 1.f);
                    rx_buf[c][i] = Get24(p) * gain;
                }
                rx_last[c] = rx_buf[c][size - 1];
            }
            rx_gain = fminf(rx_gain + size * kFadeInStep, 1.f);
            return rx_ptr;
        }

        void Send(const float *const *to_host, size_t size)
        {
            PickUpRequests();
            tu_fifo_t *ff = tud_audio_get_ep_in_ff(); // null until configured
            if(!tx_on || !ff)
                return;

            uint8_t buf[kMaxBlock * kTxFrameBytes];
            for(size_t done = 0; done < size; done += kMaxBlock)
            {
                const size_t n = size - done < kMaxBlock ? size - done : kMaxBlock;
                const uint16_t len = n * kTxFrameBytes;
                if(tu_fifo_remaining(ff) < len)
                    return;

                for(size_t c = 0; c < kTxChannels; c++)
                {
                    const float *x = to_host[c] ? to_host[c] + done : nullptr;
                    uint8_t *p = buf + c * kBytes;
                    for(size_t i = 0; i < n; i++, p += kTxFrameBytes)
                        Put24(p, x ? x[i] : 0.f);
                }
                tud_audio_write(buf, len);
            }
        }
    }

    namespace usb
    {
        // the card as a drive: TinyUSB asks in its task, the card task moves the sectors, then the answer is ready
        static uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) card_buf[CFG_TUD_MSC_EP_BUFSIZE];
        enum class CardIo : uint8_t { kIdle, kPosted, kRunning, kDone, kFailed };
        static volatile CardIo card_io = CardIo::kIdle;
        static volatile bool card_on = false, card_ejected = false;
        static uint32_t card_lba, card_count;
        static bool card_write;

        void CardMode(bool on)
        {
            card_on = on;
            card_ejected = false;
        }

        bool InCardMode() { return card_on; }

        bool TakeCardEjected()
        {
            const bool ejected = card_ejected;
            card_ejected = false;
            return ejected;
        }

        void CardTask()
        {
            if(card_io == CardIo::kPosted && !card_on) // the mode ended first
                card_io = CardIo::kIdle;
            else if(card_io == CardIo::kPosted)
            {
                card_io = CardIo::kRunning;
                if(!sd_dma.Sectors(card_lba, card_count, card_buf, card_write))
                    card_io = CardIo::kFailed;
            }
            else if(card_io == CardIo::kRunning && !sd_dma.Active())
                card_io = sd_dma.Failed() ? CardIo::kFailed : CardIo::kDone;
        }

        // posts the transfer, then busy until it's done; bytes moved, or -1
        static int32_t CardTransfer(uint32_t lba, uint8_t *buffer, uint32_t bytes, bool write)
        {
            const uint32_t count = bytes / 512;
            if(!card_on || !count || bytes > sizeof(card_buf))
                return -1;
            if(card_io == CardIo::kIdle)
            {
                card_lba = lba;
                card_count = count;
                card_write = write;
                if(write)
                    memcpy(card_buf, buffer, bytes);
                card_io = CardIo::kPosted;
            }
            else if(card_io == CardIo::kDone || card_io == CardIo::kFailed)
            {
                const bool same = card_lba == lba && card_count == count && card_write == write;
                const bool ok = card_io == CardIo::kDone;
                card_io = CardIo::kIdle; // another transfer: the host gave the last one up, this one posts next time
                if(same && ok && !write)
                    memcpy(buffer, card_buf, bytes);
                if(same)
                    return ok ? int32_t(bytes) : -1;
            }
            return TUD_MSC_RET_BUSY;
        }
    }

    static MidiTinyUsbTransport::MidiRxParseCallback midi_rx_cb = nullptr;
    static void *midi_rx_ctx = nullptr;

    // filled from any context with interrupts off, drained in Process()
    static constexpr size_t kMidiTxSize = 256;
    static uint8_t midi_tx[kMidiTxSize];
    static volatile size_t midi_tx_head = 0, midi_tx_tail = 0;
    static volatile bool midi_tx_drop = false; // only Process() moves the tail

    void MidiTinyUsbTransport::Reset() { midi_tx_drop = true; }

    void MidiTinyUsbTransport::StartRx(MidiRxParseCallback callback, void *context)
    {
        midi_rx_ctx = context;
        midi_rx_cb = callback;
    }

    bool MidiTinyUsbTransport::Tx(uint8_t *buffer, size_t size)
    {
        daisy::ScopedIrqBlocker block;
        const bool fits = (midi_tx_head - midi_tx_tail) + size <= kMidiTxSize;
        if(fits)
            for(size_t i = 0; i < size; i++)
                midi_tx[midi_tx_head++ % kMidiTxSize] = buffer[i];
        return fits;
    }

    void MidiTinyUsbTransport::Process()
    {
        uint8_t buf[64];
        while(tud_midi_available())
        {
            const uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
            if(n == 0)
                break;
            if(midi_rx_cb)
                midi_rx_cb(buf, n, midi_rx_ctx);
        }

        // reset, or nobody listening: drop the queue
        if(midi_tx_drop || !tud_midi_mounted())
        {
            midi_tx_drop = false;
            midi_tx_tail = midi_tx_head;
            return;
        }

        while(midi_tx_tail != midi_tx_head)
        {
            const size_t pos = midi_tx_tail % kMidiTxSize;
            const size_t count = midi_tx_head - midi_tx_tail;
            const size_t chunk = count < kMidiTxSize - pos ? count : kMidiTxSize - pos;
            const uint32_t sent = tud_midi_stream_write(0, &midi_tx[pos], chunk);
            midi_tx_tail += sent;
            if(sent < chunk)
                break; // the rest next time
        }
    }
}

// TinyUSB's callbacks
extern "C"
{
    void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
    {
        (void)lun;
        memcpy(vendor_id, "CHOMPI  ", 8);
        memcpy(product_id, "SD card         ", 16);
        memcpy(product_rev, "1.0 ", 4);
    }

    bool tud_msc_test_unit_ready_cb(uint8_t lun)
    {
        if(chompi::usb::card_on)
            return true;
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3a, 0x00); // medium not present
        return false;
    }

    void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
    {
        (void)lun;
        *block_count = chompi::usb::card_on ? hsd1.SdCard.LogBlockNbr : 0;
        *block_size = 512;
    }

    bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
    {
        (void)lun;
        (void)power_condition;
        if(load_eject && !start)
            chompi::usb::card_ejected = true;
        return true;
    }

    int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
    {
        (void)lun;
        return offset ? -1 : chompi::usb::CardTransfer(lba, static_cast<uint8_t *>(buffer), bufsize, false);
    }

    int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
    {
        (void)lun;
        return offset ? -1 : chompi::usb::CardTransfer(lba, buffer, bufsize, true);
    }

    int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
    {
        (void)scsi_cmd;
        (void)buffer;
        (void)bufsize;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
        return -1;
    }

    // the USB task, and a watch on main: while main's loop doesn't come round, the stacked frame shows what ran
    void usb_tick(const uint32_t *frame)
    {
        TIM7->SR = 0;
        chompi::usb::Task();
        static uint32_t ticks = 0, seen = 0, quiet = 0;
        if(++ticks % 1000) // 4 Hz
            return;
        if(chompi::usb::main_alive != seen)
        {
            seen = chompi::usb::main_alive;
            quiet = 0;
            return;
        }
        if(++quiet < 4) // a second
            return;
        chompi::usb::Log("STUCK %lus: pc %08lx lr %08lx sp %08lx xpsr %08lx r0 %08lx r1 %08lx r2 %08lx r3 %08lx r12 %08lx",
                         (unsigned long)(quiet / 4), (unsigned long)frame[6], (unsigned long)frame[5],
                         (unsigned long)(reinterpret_cast<uint32_t>(frame) + 32), (unsigned long)frame[7],
                         (unsigned long)frame[0], (unsigned long)frame[1], (unsigned long)frame[2], (unsigned long)frame[3],
                         (unsigned long)frame[4]);
    }
    __attribute__((naked)) void TIM7_IRQHandler(void)
    {
        __asm volatile("tst lr, #4\n"
                       "ite eq\n"
                       "mrseq r0, msp\n"
                       "mrsne r0, psp\n"
                       "push {r4, lr}\n"
                       "bl usb_tick\n"
                       "pop {r4, pc}\n");
    }

    // a fault: its registers to the log, then the USB serviced by hand forever, since nothing below runs
    void usb_fault(const uint32_t *frame, uint32_t which)
    {
        chompi::usb::Log("FAULT %lu: pc %08lx lr %08lx sp %08lx xpsr %08lx cfsr %08lx hfsr %08lx mmfar %08lx bfar %08lx",
                         (unsigned long)which, (unsigned long)frame[6], (unsigned long)frame[5],
                         (unsigned long)(reinterpret_cast<uint32_t>(frame) + 32), (unsigned long)frame[7],
                         (unsigned long)SCB->CFSR, (unsigned long)SCB->HFSR, (unsigned long)SCB->MMFAR,
                         (unsigned long)SCB->BFAR);
        chompi::usb::Log("FAULT regs: r0 %08lx r1 %08lx r2 %08lx r3 %08lx r12 %08lx", (unsigned long)frame[0],
                         (unsigned long)frame[1], (unsigned long)frame[2], (unsigned long)frame[3], (unsigned long)frame[4]);
        for(;;)
        {
            tusb_int_handler(0, true);
            tud_task();
            tud_cdc_write_flush();
        }
    }
#define FAULT_ENTRY(n)                    \
    __asm volatile("tst lr, #4\n"         \
                   "ite eq\n"             \
                   "mrseq r0, msp\n"      \
                   "mrsne r0, psp\n"      \
                   "mov r1, #" #n "\n"    \
                   "b usb_fault\n")
    __attribute__((naked)) void HardFault_Handler(void) { FAULT_ENTRY(1); }
    __attribute__((naked)) void MemManage_Handler(void) { FAULT_ENTRY(2); }
    __attribute__((naked)) void BusFault_Handler(void) { FAULT_ENTRY(3); }
    __attribute__((naked)) void UsageFault_Handler(void) { FAULT_ENTRY(4); }

    void OTG_HS_IRQHandler(void)
    {
        tusb_int_handler(CHOMPI_USB_RHPORT, true);
    }

    // after SET_CONFIGURATION every interface is at alt 0
    void tud_mount_cb(void)
    {
        chompi::usb::alt_rx = chompi::usb::alt_tx = false;
        chompi::usb::PostStop();
    }

    void tud_umount_cb(void) { chompi::usb::PostStop(); }

    void tud_suspend_cb(bool remote_wakeup_en)
    {
        (void)remote_wakeup_en;
        chompi::usb::PostStop();
    }

    void tud_resume_cb(void)
    {
        chompi::usb::Post(chompi::usb::req_rx, chompi::usb::alt_rx);
        chompi::usb::Post(chompi::usb::req_tx, chompi::usb::alt_tx);
    }

    bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
    {
        (void)rhport;
        const uint8_t itf = tu_u16_low(p_request->wIndex);
        const bool on = tu_u16_low(p_request->wValue) != 0;

        if(itf == ITF_NUM_AUDIO_STREAMING_RX)
            chompi::usb::Post(chompi::usb::req_rx, chompi::usb::alt_rx = on);
        else if(itf == ITF_NUM_AUDIO_STREAMING_TX)
            chompi::usb::Post(chompi::usb::req_tx, chompi::usb::alt_tx = on);
        return true;
    }

    bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
    {
        return tud_audio_set_itf_cb(rhport, p_request);
    }

    void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
    {
        (void)func_id;
        (void)alt_itf;
        // the host follows CHOMPI's clock
        feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
        feedback_param->sample_freq = CHOMPI_USB_SAMPLE_RATE;
        feedback_param->fifo_count.fifo_threshold = chompi::usb::kRxPrime;
    }

    // one fixed clock: 48 kHz, always valid
    bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
    {
        const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);

        if(entity != UAC2_ENTITY_CLOCK)
            return false;

        if(ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ)
        {
            if(p_request->bRequest == AUDIO20_CS_REQ_CUR)
            {
                audio20_control_cur_4_t cur = {(int32_t)tu_htole32(CHOMPI_USB_SAMPLE_RATE)};
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &cur, sizeof(cur));
            }
            if(p_request->bRequest == AUDIO20_CS_REQ_RANGE)
            {
                audio20_control_range_4_n_t(1) range = {};
                range.wNumSubRanges = tu_htole16(1);
                range.subrange[0].bMin = (int32_t)tu_htole32(CHOMPI_USB_SAMPLE_RATE);
                range.subrange[0].bMax = (int32_t)tu_htole32(CHOMPI_USB_SAMPLE_RATE);
                range.subrange[0].bRes = 0;
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &range, sizeof(range));
            }
        }
        else if(ctrl_sel == AUDIO20_CS_CTRL_CLK_VALID && p_request->bRequest == AUDIO20_CS_REQ_CUR)
        {
            audio20_control_cur_1_t valid = {1};
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &valid, sizeof(valid));
        }
        return false;
    }

    bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf)
    {
        (void)rhport;
        const uint8_t entity = TU_U16_HIGH(p_request->wIndex);
        const uint8_t ctrl_sel = TU_U16_HIGH(p_request->wValue);

        return entity == UAC2_ENTITY_CLOCK && ctrl_sel == AUDIO20_CS_CTRL_SAM_FREQ
               && p_request->bRequest == AUDIO20_CS_REQ_CUR && p_request->wLength == 4
               && tu_unaligned_read32(buf) == CHOMPI_USB_SAMPLE_RATE;
    }
}
