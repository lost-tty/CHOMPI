#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <stdint.h>

namespace chompi
{
    // A point being set plays for a moment once its knob rests; turning again stops it and waits anew.
    class Audition
    {
      public:
        enum class Event : uint8_t { kNone, kStart, kStop };

        // the knob moved point what (0: nothing to play), to play for play ms; a play under way is stopped by
        // the caller first
        void Turn(int what, uint32_t now, uint32_t play = 0)
        {
            what_ = what;
            play_ = play;
            pending_ = what != 0;
            on_ = false;
            at_ = now + kSettle;
        }

        // kStart and kStop are for What()
        Event Update(uint32_t now)
        {
            if(int32_t(now - at_) < 0)
                return Event::kNone;
            if(pending_)
            {
                pending_ = false;
                on_ = true;
                at_ = now + play_;
                return Event::kStart;
            }
            if(on_)
            {
                on_ = false;
                return Event::kStop;
            }
            return Event::kNone;
        }

        bool On() const { return on_; }
        int What() const { return what_; }

      private:
        static constexpr uint32_t kSettle = 80; // ms

        int what_ = 0;
        uint32_t play_ = 0;
        bool pending_ = false, on_ = false;
        uint32_t at_ = 0;
    };
}

#pragma GCC pop_options
