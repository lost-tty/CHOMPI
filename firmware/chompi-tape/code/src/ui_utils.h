#pragma once
#include "hardware.h"

/** Needed for the Flush/Clear LEDs functions */
extern chompi::Hardware hw;

void FlushLeds(const daisy::UiCanvasDescriptor& canvasDescriptor)
{
    // hw.UpdateLeds();
}
void ClearLeds(const daisy::UiCanvasDescriptor& canvasDescriptor)
{
    // hw.ClearLeds();
}

// Crossfades between LED colours.
inline float color_xfade(float start, float end, float idx) { return (1.f - idx) * start + idx * end; }
inline float color_triple_xfade(float start, float mid, float end, float idx)
{
    return idx < .5f ? color_xfade(start, mid, idx * 2.f) : color_xfade(mid, end, (idx - .5f) * 2.f);
}
inline float color_quad_xfade(float start, float mid1, float mid2, float end, float idx)
{
    if(idx < .33f)
        return color_xfade(start, mid1, idx * 3.f);
    if(idx < .66f)
        return color_xfade(mid1, mid2, (idx - .33f) * 3.f);
    return color_xfade(mid2, end, (idx - .66f) * 3.f);
}
