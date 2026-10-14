#pragma once

#include <stdint.h>
#include "Hold.h"

namespace chompi
{
    // a knob's click: held it does one thing, turned while pressed another, a plain click a third
    class PressKnob
    {
      public:
        void Init()
        {
            hold_.Init();
            turned_ = false;
        }
        // true for the plain click
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
        // a turn: true while pressed, which then is neither a click nor a hold
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
