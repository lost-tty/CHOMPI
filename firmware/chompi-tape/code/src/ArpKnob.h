#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <stdint.h>
#include "Arp.h"

namespace chompi
{
    // The arp page of the left knob.
    class ArpKnob
    {
      public:
        void Init() { ticks_ = 0; }

        void Latch() { arp.SetLatch(!arp.Latched()); }

        // press says the knob was turned while pressed
        void Turn(int turns, bool shift, bool press)
        {
            const int dir = turns > 0 ? 1 : -1;
            if(!shift && !press && arp.Note() == Arp::kFree && (dir > 0 || arp.Slow() > 0.f))
            {
                arp.SetSlow(arp.Slow() + dir * .025f);
                return;
            }
            if(!TwoDetents(dir))
                return;
            if(shift)
                arp.SetOctaves(arp.Octaves() + dir);
            else if(press)
                arp.SetPattern(arp.Pattern() + dir);
            else
                arp.SetNote(arp.Note() + dir);
        }

        void Off()
        {
            arp.SetNote(-1);
            arp.SetLatch(false);
        }

        void Led(bool shift, bool pattern, uint32_t now, float &r, float &g, float &b) const
        {
            static const float kNoteColors[Arp::kNotes][3] = {
                {0.f, 1.f, 0.f}, {1.f, .95f, .05f}, {1.f, .6f, .24f}, {1.f, .36f, .62f},
                {1.f, 0.f, 0.f}, {1.f, 0.f, 1.f}, {0.f, 0.f, 1.f},
            };
            static const float kPatternColors[Arp::kPatterns][3] = {
                {1.f, 1.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, .84f, 1.f}, {0.f, 1.f, 0.f},
                {1.f, 0.f, 1.f}, {.58f, .05f, 1.f}, {.706f, 1.f, 0.f}, {.14f, 1.f, .92f},
            };
            static const float kWhite[3] = {1.f, 1.f, 1.f};

            const int n = arp.Note();
            const float* c = pattern ? kPatternColors[arp.Pattern()] : n >= 0 && n < Arp::kNotes ? kNoteColors[n] : kWhite;
            float k = shift ? arp.Octaves() / 3.f : pattern ? 1.f : n < 0 ? .1f : n < Arp::kNotes ? 1.f : 1.f - .8f * arp.Slow();
            if(shift)
                c = kWhite;
            else if(arp.Latched())
                k = (now / 400) % 2 ? k * .3f : n >= 0 ? k : .6f;
            r = k * c[0];
            g = k * c[1];
            b = k * c[2];
        }

      private:
        bool TwoDetents(int dir)
        {
            ticks_ = (ticks_ > 0) == (dir > 0) ? ticks_ + dir : dir;
            if(ticks_ != 2 && ticks_ != -2)
                return false;
            ticks_ = 0;
            return true;
        }

        int8_t ticks_;
    };

    extern ArpKnob arp_knob;
}

#pragma GCC pop_options
