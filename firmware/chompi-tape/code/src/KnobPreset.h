#pragma once

#include "PresetManager.h"
#include "DSPEngine.h"

namespace chompi
{
    // Fills the knob rows from a preset: page 0 pitch; page 1 gain, attack, decay. The points are the engine's.
    inline void KnobsFromPreset(const PresetManager::Preset &p, float **enc)
    {
        enc[0][0] = p.pitch;
        enc[1][0] = p.gain;
        enc[1][1] = p.attack;
        enc[1][2] = p.decay;
    }
    inline PresetManager::Preset PresetFromKnobs(float **enc, daisy::Engine &fx)
    {
        return {enc[0][0], fx.SamplePoint(0), fx.SamplePoint(1), enc[1][1], enc[1][2], enc[1][0], fx.GetPan(),
                fx.Glide(), fx.GetAutoLoop(), fx.GetSustainActive(), fx.Legato()};
    }
}
