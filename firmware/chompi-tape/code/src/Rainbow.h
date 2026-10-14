#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once

#include <algorithm>
#include "Leds.h"

namespace chompi
{
    // The boot animation: a rainbow rolls over the LEDs from power-on, in the LEDs' boot frame; once the UI
    // draws, it fades into the UI over a second and stops.
    class Rainbow
    {
      public:
        // from any tick; ui_up: the pages draw now. True when it drew a frame
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

            gain_ = std::min(1.f, gain_ + .02f);
            idx_ += kInc;
            if(idx_ >= 7.f)
                idx_ -= 7.f;

            float fidx = idx_ + 1.5f;
            if(fidx >= 7.f)
                fidx -= 7.f;
            for(int i = 0; i < LedFrame::kPth; i++)
            {
                const float fgain = daisysp::fclamp(gain_ * 10.f - (9.f - i), 0.f, 1.f);
                Led(fidx, fgain, i, true);
                fidx = Next(fidx, kPthStep);
            }
            fidx = idx_;
            for(int i = 0; i < 10; i++) // the black keys
            {
                const float fgain = daisysp::fclamp(gain_ * 10.f - (9.f - i), 0.f, 1.f);
                Led(fidx, fgain, i, false);
                fidx = Next(fidx, kSmtStepBlack);
            }
            fidx = idx_;
            for(int i = 0; i < 15; i++) // the white keys
            {
                const float fgain = daisysp::fclamp(gain_ * 15.f - (14.f - i), 0.f, 1.f);
                Led(fidx, fgain, 24 - i, false);
                fidx = Next(fidx, kSmtStepWhite);
            }
            return true;
        }

        void Stop() { leds.boot_mix = 0; }
        bool Running() const { return leds.boot_mix != 0; }

      private:
        static constexpr uint32_t kFrame = 16, kFade = 1000; // ms: a step, and the fade into the UI
        static constexpr float kInc = .1f, kPthStep = .27f, kSmtStepBlack = .6f, kSmtStepWhite = .4f;

        // roygbiv, and red again so the roll-over interpolates
        static void Led(float fidx, float fgain, int i, bool pth)
        {
            static const uint8_t reds[8] = {255, 255, 255, 0, 0, 75, 238, 255};
            static const uint8_t greens[8] = {0, 146, 255, 255, 0, 0, 130, 0};
            static const uint8_t blues[8] = {0, 0, 0, 0, 255, 130, 238, 0};
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
            return fidx >= 7.f ? fidx - 7.f : fidx;
        }

        float idx_ = 0.f, gain_ = 0.f;
        uint32_t stepped_ = 0, lift_ = 0;
        bool lifted_ = false;
    };
}

#pragma GCC pop_options
