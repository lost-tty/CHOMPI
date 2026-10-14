#pragma once

#include <stddef.h>
#include "Colors.h"

namespace chompi
{
    // presets.json holds the banks by position, so a new one goes at the end.
    struct Bank
    {
        char letter;
        const float *color;
    };

    static const Bank kBanks[] = {
        {'a', purple},
        {'b', orange},
        {'c', teal},
        {'d', dark_orange},
        {'e', yellow_green},
    };
    static constexpr size_t kNumBanks = sizeof(kBanks) / sizeof(kBanks[0]);

    // Returns the bank with the letter, kNumBanks for none.
    inline size_t BankOfLetter(char letter)
    {
        size_t b = 0;
        while(b < kNumBanks && kBanks[b].letter != letter)
            b++;
        return b;
    }
}
