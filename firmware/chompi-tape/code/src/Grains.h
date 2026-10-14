#pragma once

#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <algorithm>
#include "daisysp.h"
#include "InterpolatedDelayLine.h"

namespace chompi
{
    // A cloud of grains from the last seconds, between the delay and the reverb.
    // Audio thread; SetAmount, SetSize and the freeze also from the UI.
    class Grains
    {
      public:
        static constexpr size_t kFrames = 1 << 17; // 2.7 s
        static constexpr int kGrains = 8;
        using Frame = InterpolatedDelayLine::AudioSample;

        void Init(Frame *mem)
        {
            mem_ = mem;
            std::fill_n(mem, kFrames, Frame{});
            write_ = 0;
            amt_ = amt_target_ = 0.f;
            size_ = .5f;
            wait_ = 0.f;
            seed_ = 1;
            turn_ = 0;
            note_ = quarter_ = step_ = own_ = 0.f;
            pos_ = -1.f;
            tick_ = 0;
            frozen_ = sealed_ = false;
            for(Grain &g : grains_)
                g.left = 0;
        }

        // 0 off, 1 a dense, wide cloud
        void SetAmount(float amt) { amt_target_ = amt; }
        // 0..1, 25 to 500 ms
        void SetSize(float size) { size_ = size; }
        void Snap() { amt_ = amt_target_; }
        // audio thread, once a block while the delay is on a note value, else note 0: the note in quarters,
        // samples per quarter, and the playhead in quarters, < 0 without one
        void SetGrid(float note, float quarter, float pos)
        {
            note_ = note;
            quarter_ = quarter;
            pos_ = pos;
        }
        bool Frozen() const { return frozen_; }
        void ToggleFreeze() { frozen_ = !frozen_; }
        void Thaw() { frozen_ = false; }

        void Process(float *outl, float *outr, size_t size) __attribute__((noinline, optimize("O2")))
        {
            // in time: the delay's note, then half and a quarter of it further up, never under 30 ms.
            // On the playhead's grid, or on its own while the playhead stands.
            bool tick = false;
            if(note_ > 0.f)
            {
                float div = amt_ < .33f ? 1.f : amt_ < .66f ? .5f : .25f;
                while(div < 1.f && note_ * div * quarter_ < 1440.f)
                    div *= 2.f;
                own_ = pos_ >= 0.f ? pos_ : own_ + size / quarter_;
                if(own_ >= 3072.f) // quarters, a multiple of every note * div
                    own_ -= 3072.f;
                const int32_t t = static_cast<int32_t>(floorf(own_ / (note_ * div)));
                tick = t != tick_;
                tick_ = t;
                step_ = note_ * div * quarter_;
            }

            if(frozen_ != sealed_)
            {
                sealed_ = frozen_;
                Seal(!sealed_);
            }

            for(size_t i = 0; i < size; i++)
            {
                daisysp::fonepole(amt_, amt_target_, .001f);
                if(amt_ < .001f) // keeps the buffer fresh for the turn up
                {
                    if(!frozen_)
                    {
                        mem_[write_] = {S16(outl[i]), S16(outr[i])};
                        write_ = (write_ + 1) & (kFrames - 1);
                    }
                    continue;
                }

                if(note_ > 0.f ? tick && !i : (wait_ -= 1.f) <= 0.f)
                    Spawn();

                float wl = 0.f, wr = 0.f;
                for(Grain &g : grains_)
                {
                    if(!g.left)
                        continue;
                    const float e = g.phase * (1.f - g.phase);
                    const float env = e * e; // round, close to a Hann window
                    const int32_t p = static_cast<int32_t>(g.pos);
                    const float f = static_cast<float>(g.pos - p);
                    const Frame a = mem_[p & (kFrames - 1)], b = mem_[(p + 1) & (kFrames - 1)];
                    wl += (a.l + (b.l - a.l) * f) * env * g.gl;
                    wr += (a.r + (b.r - a.r) * f) * env * g.gr;
                    g.pos += g.inc;
                    if(g.pos < 0.f)
                        g.pos += kFrames;
                    else if(g.pos >= kFrames)
                        g.pos -= kFrames;
                    g.phase += g.dphase;
                    g.left--;
                }
                wl *= 1.f / 32768.f;
                wr *= 1.f / 32768.f;

                // a little feedback smears the cloud at the top
                if(!frozen_)
                {
                    const float fb = .35f * amt_ * amt_;
                    mem_[write_] = {S16(outl[i] + wl * fb), S16(outr[i] + wr * fb)};
                    write_ = (write_ + 1) & (kFrames - 1);
                }

                // the grains come in over the first 2/3
                const float wet = std::min(1.f, amt_ * 1.5f) * .85f;
                const float dry = 1.f - .6f * wet;
                outl[i] = outl[i] * dry + wl * wet;
                outr[i] = outr[i] * dry + wr * wet;
            }
        }

      private:
        struct Grain
        {
            double pos;        // frames, a float would round the detune away up the buffer
            float inc;
            float phase, dphase;
            float gl, gr;
            uint32_t left;
        };

        // clamps a NaN too, unlike f2s16
        static int16_t S16(float x) { return static_cast<int16_t>(std::max(-1.f, std::min(1.f, x)) * 32767.f); }

        float Random()
        {
            seed_ ^= seed_ << 13;
            seed_ ^= seed_ >> 17;
            seed_ ^= seed_ << 5;
            return (seed_ >> 8) * (1.f / 16777216.f);
        }

        // a freeze stops the write head, a thaw moves it again: grains that would read across it end before
        void Seal(bool writing)
        {
            for(Grain &g : grains_)
            {
                const float rate = g.inc > 0.f ? g.inc - writing : writing - g.inc; // frames closer per frame
                if(!g.left || rate <= 0.f)
                    continue;
                const uint32_t p = static_cast<uint32_t>(g.pos);
                const uint32_t gap = (g.inc > 0.f ? write_ - p : p - write_) & (kFrames - 1);
                const uint32_t room = static_cast<uint32_t>(gap / rate);
                const uint32_t reach = room > 2 ? room - 2 : 1;
                if(g.left > reach)
                {
                    g.dphase = (1.f - g.phase) / reach;
                    g.left = reach;
                }
            }
        }

        // denser, wider and more often shifted the further up
        void Spawn() __attribute__((noinline, optimize("Os"))) // a few times a second
        {
            const float n = 1200.f + size_ * size_ * 22800.f;
            float overlap = 2.f + 2.f * amt_;
            if(note_ > 0.f)
                overlap = std::max(1.f, std::min(float(kGrains), n / step_));
            else
                wait_ = n / overlap * (.5f + Random());

            Grain *g = nullptr;
            for(Grain &c : grains_)
                if(!c.left)
                    g = &c;
            if(!g)
                return;

            // the grains take turns through a chord that grows: as played, then an octave up,
            // an octave down, a fifth. Shifted grains sit a little behind.
            static const float kChord[8] = {1.f, 1.f, 2.f, 1.f, .5f, 2.f, 1.5f, 2.f};
            const int notes = amt_ < .33f ? 1 : std::min(8, 3 + static_cast<int>((amt_ - .33f) * 7.5f));
            const float shift = kChord[turn_++ % notes];
            float inc = shift * (1.f + (Random() - .5f) * .006f);
            if(Random() < .4f * (amt_ - .6f)) // the odd reverse near the top
                inc = -inc;

            // back far enough not to run into the write, or into the seam of a frozen buffer
            const float ahead = std::max(0.f, n * (inc - (frozen_ ? 0.f : 1.f)));
            const float back = 64.f + ahead + Random() * (.02f + .48f * amt_ * amt_) * kFrames; // close behind at first
            const float reach = fabsf(inc) + (inc < 0.f && !frozen_ ? 1.f : 0.f); // a reverse grain meets the writer
            g->pos = write_ - std::min(back, kFrames - n * reach - 64.f);
            if(g->pos < 0.f)
                g->pos += kFrames;
            g->inc = inc;
            g->phase = 0.f;
            g->dphase = 1.f / n;
            const float pan = (Random() - .5f) * 1.4f * amt_;
            const float gain = (shift == 1.f ? 18.f : 12.f) / sqrtf(overlap); // about as loud as the input
            g->gl = gain * (1.f - pan);
            g->gr = gain * (1.f + pan);
            g->left = static_cast<uint32_t>(n);
        }

        Frame *mem_;
        uint32_t write_;
        volatile float amt_target_, size_;
        float amt_;
        float wait_;
        uint32_t seed_;
        uint32_t turn_;
        float note_, quarter_, pos_;
        float step_; // samples
        float own_;  // quarters
        int32_t tick_;
        volatile bool frozen_;
        bool sealed_; // the freeze the grains were last fitted to
        Grain grains_[kGrains];
    };

    extern Grains grains;
}
