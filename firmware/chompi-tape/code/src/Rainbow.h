#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once

#include <algorithm>
#include "Leds.h"

namespace chompi
{
    // Rolls a rainbow over the LEDs from power-on, in the LEDs' boot frame. Once the UI draws, it fades into
    // the UI over a second and stops.
    class Rainbow
    {
      public:
        // Steps from any tick; ui_up says the pages draw now. Returns true when it drew a frame.
        bool Step(uint32_t now, bool ui_up)
        {
            if(!leds.boot_mix || now - stepped_ < kFrame)
                return false;
            stepped_ = now;
            if(ui_up && !lifted_)
            {
                lifted_ = true;
                lift_ = now;
            }
            if(lifted_)
                leds.boot_mix = now - lift_ >= kFade ? 0 : 255 - (now - lift_) * 255 / kFade;
            if(!leds.boot_mix)
                return false;

            gain_ = std::min(1.f, gain_ + kGainStep);
            idx_ += kInc;
            if(idx_ >= kColours)
                idx_ -= kColours;

            float fidx = idx_ + 1.5f;
            if(fidx >= kColours)
                fidx -= kColours;
            for(int i = 0; i < LedFrame::kPth; i++)
            {
                const float fgain = daisysp::fclamp(gain_ * LedFrame::kPth - (LedFrame::kPth - 1.f - i), 0.f, 1.f);
                Led(fidx, fgain, i, true);
                fidx = Next(fidx, kPthStep);
            }
            fidx = idx_;
            for(int i = 0; i < kBlackKeys; i++)
            {
                const float fgain = daisysp::fclamp(gain_ * kBlackKeys - (kBlackKeys - 1.f - i), 0.f, 1.f);
                Led(fidx, fgain, i, false);
                fidx = Next(fidx, kSmtStepBlack);
            }
            fidx = idx_;
            for(int i = 0; i < kWhiteKeys; i++)
            {
                const float fgain = daisysp::fclamp(gain_ * kWhiteKeys - (kWhiteKeys - 1.f - i), 0.f, 1.f);
                Led(fidx, fgain, LedFrame::kSmt - 1 - i, false);
                fidx = Next(fidx, kSmtStepWhite);
            }
            return true;
        }

        void Stop() { leds.boot_mix = 0; }

        bool Running() const { return leds.boot_mix != 0; }

      private:
        static constexpr uint32_t kFrame = 16, kFade = 1000; // ms: a step, and the fade into the UI
        static constexpr float kInc = .1f, kPthStep = .27f, kSmtStepBlack = .6f, kSmtStepWhite = .4f;
        static constexpr float kGainStep = .02f;
        static constexpr int kColours = 7; // roygbiv; the table has red again after them
        static constexpr int kBlackKeys = 10, kWhiteKeys = 15;

        // The table runs roygbiv and red again so the roll-over interpolates.
        static void Led(float fidx, float fgain, int i, bool pth)
        {
            static const uint8_t reds[kColours + 1] = {255, 255, 255, 0, 0, 75, 238, 255};
            static const uint8_t greens[kColours + 1] = {0, 146, 255, 255, 0, 0, 130, 0};
            static const uint8_t blues[kColours + 1] = {0, 0, 0, 0, 255, 130, 238, 0};
            const size_t floor = fidx;
            const float frac = fidx - floor, g = fgain * fgain;
            const uint8_t r = g * (frac * (reds[floor + 1] - reds[floor]) + reds[floor]);
            const uint8_t gr = g * (frac * (greens[floor + 1] - greens[floor]) + greens[floor]);
            const uint8_t b = g * (frac * (blues[floor + 1] - blues[floor]) + blues[floor]);
            if(pth)
                leds.boot.Pth(i, r, gr, b);
            else
                leds.boot.Smt(i, r, gr, b);
        }

        static float Next(float fidx, float step)
        {
            fidx += step;
            return fidx >= kColours ? fidx - kColours : fidx;
        }

        float idx_ = 0.f, gain_ = 0.f;
        uint32_t stepped_ = 0, lift_ = 0;
        bool lifted_ = false;
    };
}

#pragma GCC pop_options
