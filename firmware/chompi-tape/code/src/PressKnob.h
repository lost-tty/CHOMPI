#pragma once

#include <stdint.h>
#include "Hold.h"

namespace chompi
{
    // Tells a knob's click apart from a hold and a turn while pressed.
    class PressKnob
    {
      public:
        void Init()
        {
            hold_.Init();
            turned_ = false;
        }

        // Returns true for the plain click.
        bool Click(bool down, uint32_t now)
        {
            if(!down)
                return hold_.Release();
            hold_.Press(now);
            turned_ = false;
            return false;
        }

        bool Held(uint32_t now) { return hold_.Held(now); }

        bool Down() const { return hold_.Down(); }

        // Notes a turn. Returns true while pressed, and the press then counts as neither a click nor a hold.
        bool Turning()
        {
            if(!hold_.Down())
                return false;
            turned_ = true;
            hold_.Cancel();
            return true;
        }

        bool Turned() const { return hold_.Down() && turned_; }

        void Lost() { hold_.Lost(); }

      private:
        Hold hold_;
        bool turned_ = false;
    };
}
