#pragma once
namespace daisy
{
    // the host has no interrupts to hold off
    struct ScopedIrqBlocker
    {
        ScopedIrqBlocker() {}
        ~ScopedIrqBlocker() {}
    };
}
