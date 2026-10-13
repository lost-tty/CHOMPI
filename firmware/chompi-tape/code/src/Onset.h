#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include <cmath>
#include "daisy_core.h"

namespace chompi
{
    static constexpr size_t kNoOnset = ~size_t(0);
    static constexpr size_t kOnsetHop = 2 * 48;     // 1 ms of stereo samples
    static constexpr size_t kOnsetWarm = 2 * 2400;  // the baseline settles over this before the window
    static constexpr float kOnsetFloor = 1e-4f;     // -40 dBFS, mean square
    static constexpr float kOnsetRise = 4.f;        // 6 dB over the baseline
    static constexpr float kOnsetBaseline = .0625f; // the baseline's step per ms
    static constexpr float kOnsetHighpass = .8f;    // ~1.5 kHz

    // The onset nearest to at within the stereo int16 samples [lo, hi) of [begin, end), as a sample index.
    // Per ms, the energy full band and above the highpass against a running baseline of each: an onset is
    // new energy in either, over the baseline by the rise and above the floor; a run of such milliseconds
    // counts at its loudest one
    inline size_t Onset(const int16_t *mem, size_t begin, size_t end, size_t lo, size_t hi, size_t at)
    {
        lo = std::max(begin, lo) & ~size_t(1);
        hi = std::min(end, hi);
        const size_t from = std::max(begin, lo - std::min(lo, kOnsetWarm)) & ~size_t(1);
        float base[2] = {0.f, 0.f}, hp = 0.f, x1 = 0.f, run_score = 0.f;
        size_t run = kNoOnset, best = kNoOnset, best_dist = kNoOnset;
        for(size_t p = from; p + kOnsetHop <= hi; p += kOnsetHop)
        {
            float e[2] = {0.f, 0.f};
            for(size_t i = 0; i < kOnsetHop; i += 2)
            {
                const float x = (s162f(mem[p + i]) + s162f(mem[p + i + 1])) * .5f;
                hp = x - x1 + kOnsetHighpass * hp;
                x1 = x;
                e[0] += x * x;
                e[1] += hp * hp;
            }
            float score = 0.f;
            if(p >= lo)
                for(int k = 0; k < 2; k++)
                    if(e[k] >= kOnsetHop * .5f * kOnsetFloor)
                        score = std::max(score, e[k] / (base[k] + kOnsetHop * .5f * kOnsetFloor * .01f));
            if(score >= kOnsetRise)
            {
                if(run == kNoOnset || score > run_score)
                {
                    run = p;
                    run_score = score;
                }
            }
            else if(run != kNoOnset) // the run ended: a candidate
            {
                const size_t dist = run > at ? run - at : at - run;
                if(dist < best_dist)
                {
                    best = run;
                    best_dist = dist;
                }
                run = kNoOnset;
            }
            for(int k = 0; k < 2; k++)
                base[k] += (e[k] - base[k]) * kOnsetBaseline;
        }
        if(run != kNoOnset && (run > at ? run - at : at - run) < best_dist)
            best = run;
        return best;
    }

    // The loudness of the stereo int16 samples [begin, end) in n parts, 0..1 of the loudest, from every 16th frame
    inline void Profile(const int16_t *mem, size_t begin, size_t end, float *out, size_t n)
    {
        const size_t len = end - begin;
        float top = 0.f;
        for(size_t k = 0; k < n; k++)
        {
            const size_t lo = begin + (len * k / n & ~size_t(1)), hi = begin + (len * (k + 1) / n & ~size_t(1));
            float e = 0.f;
            size_t m = 0;
            for(size_t p = lo; p + 2 <= hi; p += 32, m++)
            {
                const float x = (s162f(mem[p]) + s162f(mem[p + 1])) * .5f;
                e += x * x;
            }
            out[k] = m ? sqrtf(e / m) : 0.f;
            top = std::max(top, out[k]);
        }
        for(size_t k = 0; k < n && top > 0.f; k++)
            out[k] /= top;
    }

    // A start point that snaps onto a transient ahead of it in the direction it is turned, and holds it:
    // moved away it goes freely, moved back toward it while still near it snaps again, well away it lets go.
    class Snap
    {
      public:
        // the point moved from was to at within [begin, end): where it settles
        size_t Settle(const int16_t *mem, size_t begin, size_t end, size_t was, size_t at)
        {
            if(at_ < begin || at_ >= end) // another recording's
                at_ = kNoOnset;
            if(at_ != kNoOnset)
            {
                const size_t dist = at > at_ ? at - at_ : at_ - at;
                const bool toward = (at < at_) == (at > was);
                if(dist < kHold)
                    return toward ? at_ : at;
                at_ = kNoOnset;
            }
            const bool up = at > was;
            const size_t lo = at - std::min(at, up ? kBehind : kAhead), hi = at + (up ? kAhead : kBehind);
            at_ = Onset(mem, begin, end, lo, hi, at);
            return at_ != kNoOnset ? at_ : at;
        }

        // the point sits on the transient it holds
        bool On(size_t at) const { return at_ != kNoOnset && at == at_; }

        void Drop() { at_ = kNoOnset; }

      private:
        // ahead a transient is caught from, behind for one just passed, and away before it is let go
        static constexpr size_t kAhead = 2 * 12000, kBehind = 2 * 480, kHold = 2 * 4800; // 250, 10, 100 ms

        size_t at_ = kNoOnset;
    };
}

#pragma GCC pop_options
