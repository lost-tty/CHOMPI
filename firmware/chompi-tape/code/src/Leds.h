#pragma once

#include <algorithm>
#include "daisy_seed.h"

namespace chompi
{
    // One frame of the panel's LEDs: the 10 through-hole ones (knobs, wheel, transport, CHOMPI key) and the 25 keys
    struct LedFrame
    {
        static constexpr int kPth = 10, kSmt = 25;

        uint8_t pth[kPth][3] = {}, smt[kSmt][3] = {};

        // 0..1; the two kinds of LED differ in brightness
        void Pth(int i, float r, float g, float b) { Set(pth[i], r * 255, g * 255, b * 255, 11); }
        void Smt(int i, float r, float g, float b) { Set(smt[i], r * 255, g * 255, b * 255, 4); }
        void Pth(int i, int r, int g, int b) { Set(pth[i], r, g, b, 11); }
        void Smt(int i, int r, int g, int b) { Set(smt[i], r, g, b, 4); }

        void Clear()
        {
            std::fill(&pth[0][0], &pth[0][0] + sizeof(pth), 0);
            ClearKeys();
        }
        void ClearKeys() { std::fill(&smt[0][0], &smt[0][0] + sizeof(smt), 0); }

      private:
        static void Set(uint8_t *led, int r, int g, int b, int div)
        {
            led[0] = r / div;
            led[1] = g / div;
            led[2] = b / div;
        }
    };

    // The LEDs: two frames, the pages' and the boot animation's, mixed by boot_mix on the way out. Show() encodes
    // the mix for the two WS2812 strings; their DMA runs on, string after string, from Init().
    class Leds
    {
      public:
        LedFrame ui, boot;
        uint8_t boot_mix = 255; // 255: the boot frame alone

        void Init()
        {
            daisy::TimerHandle::Config tim3_cfg, tim5_cfg;
            tim3_cfg.periph = daisy::TimerHandle::Config::Peripheral::TIM_3;
            tim5_cfg.periph = daisy::TimerHandle::Config::Peripheral::TIM_5;
            tim3_cfg.dir = daisy::TimerHandle::Config::CounterDir::UP;
            tim5_cfg.dir = daisy::TimerHandle::Config::CounterDir::UP;
            tim3_smt_.Init(tim3_cfg);
            tim5_pth_.Init(tim5_cfg);

            const uint32_t prescaler = 8;
            const uint32_t tickspeed = (daisy::System::GetPClk2Freq() * 2) / prescaler;
            const uint32_t target_pulse_freq = 833332; // 1.2 us symbols
            const uint32_t period = (tickspeed / target_pulse_freq) - 1;
            tim3_smt_.SetPrescaler(prescaler - 1);
            tim3_smt_.SetPeriod(period);
            tim5_pth_.SetPrescaler(prescaler - 1);
            tim5_pth_.SetPeriod(period);

            daisy::TimChannel::Config smt_cfg, pth_cfg;
            smt_cfg.tim = &tim3_smt_;
            smt_cfg.chn = daisy::TimChannel::Config::Channel::TWO;
            smt_cfg.mode = daisy::TimChannel::Config::Mode::PWM;
            smt_cfg.polarity = daisy::TimChannel::Config::Polarity::HIGH;
            smt_cfg.pin = daisy::seed::D18;
            pth_cfg.tim = &tim5_pth_;
            pth_cfg.chn = daisy::TimChannel::Config::Channel::FOUR;
            pth_cfg.mode = daisy::TimChannel::Config::Mode::PWM;
            pth_cfg.polarity = daisy::TimChannel::Config::Polarity::HIGH;
            pth_cfg.pin = daisy::seed::D16;
            pth_pwm_.Init(pth_cfg);
            smt_pwm_.Init(smt_cfg);

            Show();
            smt_pwm_.Start();
            smt_pwm_.StartDma(out_smt_, kOutSmt, EndOfString, this);
        }

        // Puts the strip out, the boot animation with it.
        void Off()
        {
            boot_mix = 0;
            ui.Clear();
            boot.Clear();
            Show();
        }

        // the frames as they are, into the strings' bit timings
        void Show()
        {
            for(int i = 0; i < kPthString; i++)
                Encode(&out_pth_[i * 24], i - kPorch, LedFrame::kPth, ui.pth, boot.pth, false);
            for(int i = 0; i < kSmtString; i++)
                Encode(&out_smt_[i * 24], i - kPorch, LedFrame::kSmt, ui.smt, boot.smt, true);
        }

        // the strings carry dark LEDs before and after the ones on the panel
        static constexpr int kPorch = 6;
        static constexpr int kPthString = LedFrame::kPth + 2 * kPorch, kSmtString = LedFrame::kSmt + 2 * kPorch;
        static constexpr size_t kOutPth = kPthString * 24, kOutSmt = kSmtString * 24;

      private:
        static constexpr uint32_t kOne = 20, kZero = 10; // 0.68 us and 0.334 us, measured on the rev 2 hardware

        uint8_t Mix(uint8_t a, uint8_t b) const { return (a * (255 - boot_mix) + b * boot_mix + 127) / 255; }

        // one LED: its 24 bit timings, the keys' string in GRB order
        void Encode(uint32_t *out, int k, int n, const uint8_t (*a)[3], const uint8_t (*b)[3], bool grb) const
        {
            if(k < 0 || k >= n)
            {
                std::fill(out, out + 24, 0);
                return;
            }
            const uint8_t r = Mix(a[k][0], b[k][0]), g = Mix(a[k][1], b[k][1]), bl = Mix(a[k][2], b[k][2]);
            Bits(out, grb ? g : r);
            Bits(out + 8, grb ? r : g);
            Bits(out + 16, bl);
        }
        static void Bits(uint32_t *out, uint8_t v)
        {
            for(int i = 0; i < 8; i++)
                out[i] = v & (1 << (7 - i)) ? kOne : kZero;
        }

        // one string done: the other goes
        static void EndOfString(void *context)
        {
            Leds &l = *static_cast<Leds *>(context);
            if(l.pth_done_)
            {
                l.pth_pwm_.SetPwm(0);
                l.smt_pwm_.Start();
                l.smt_pwm_.StartDma(l.out_smt_, kOutSmt, EndOfString, context);
            }
            else
            {
                l.smt_pwm_.SetPwm(0);
                l.pth_pwm_.Start();
                l.pth_pwm_.StartDma(l.out_pth_, kOutPth, EndOfString, context);
            }
            l.pth_done_ = !l.pth_done_;
        }

        daisy::TimerHandle tim3_smt_, tim5_pth_;
        daisy::TimChannel pth_pwm_, smt_pwm_;
        bool pth_done_ = false;
        uint32_t *out_pth_, *out_smt_; // the bit timings, in memory the DMA reaches
      public:
        Leds(uint32_t *out_pth, uint32_t *out_smt) : out_pth_(out_pth), out_smt_(out_smt) {}
    };

    uint32_t DMA_BUFFER_MEM_SECTION led_out_pth[Leds::kOutPth];
    uint32_t DMA_BUFFER_MEM_SECTION led_out_smt[Leds::kOutSmt];
    Leds leds(led_out_pth, led_out_smt);
}
