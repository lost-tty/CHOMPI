#pragma once

#include "daisy.h"
#include "util/scopedirqblocker.h"

namespace chompi
{
    // The beat grid: MIDI clock while it runs, else the metronome.
    // MIDI comes in the receive interrupts, above the tasks, so what reads its state from a task takes the lock.
    // The MIDI grid starts with Start or Song Position, joining a running clock would be off the beat.
    class Clock
    {
    public:
        static constexpr uint32_t kTicksPerQuarter = 24;
        static constexpr int kMinTempo = 40;
        static constexpr int kMaxTempo = 240;
        static constexpr float kBeepLevel = .2f; // the beep's peak

        void Init()
        {
            metro_on_ = beep_on_ = had_grid_ = false;
            tempo_ = 120;
            sig_ = 0;
            delay_note_ = -1;
            delay_quarter_ = 0.f;
            delay_touched_ = false;
            metro_beat_ = last_pulse_ = 0;
            metro_phase_ = beep_env_ = beep_ph_ = beep_inc_ = 0.f;
            SetSampleRate(48000.f);
            source_ = nullptr;
            started_ = running_ = false;
            spp_ = spp_lsb_ = 0;
            count_ = outliers_ = 0;
            heard_ = last_ = t_ = period_ = 0;
        }

        // every received byte, before parsing
        void Byte(uint8_t byte, const void *source)
        {
            const bool data = byte < 0x80;
            if(data ? !spp_ || source != source_ : !IsTransport(byte))
            {
                if(!data && byte < 0xF8 && source == source_) // another message cuts a song position short
                    spp_ = 0;
                return;
            }

            // the first source to send clock keeps it until it goes quiet
            const uint32_t now = daisy::System::GetTick();
            const bool quiet = now - heard_ >= Lost();
            if(source != source_ && source_ && !quiet)
                return;
            source_ = source;
            heard_ = now;
            if(quiet) // the position is unknown after a gap
                started_ = running_ = false;

            if(data) // song position, in 16ths
            {
                if(spp_ == 1)
                {
                    spp_lsb_ = byte;
                    spp_ = 2;
                }
                else
                {
                    count_ = ((byte << 7) | spp_lsb_) * 6;
                    started_ = true;
                    spp_ = 0;
                }
                return;
            }

            switch(byte)
            {
                case 0xF8: Tick(now); break;
                case 0xFA: // start: the next tick is the downbeat
                    count_ = 0;
                    started_ = running_ = true;
                    break;
                case 0xFB: running_ = started_; break; // continue
                case 0xFC: running_ = false; break;    // stop
                case 0xF2: // song position, unknown until both data bytes are in
                    started_ = running_ = false;
                    spp_ = 1;
                    break;
            }
        }

        void SetSampleRate(float sr)
        {
            sr_ = sr;
            beep_decay_ = expf(-1.f / (.012f * sr));
        }

        void SetMetronome(bool on)
        {
            daisy::ScopedIrqBlocker block;
            metro_on_ = beep_on_ = on;
            metro_beat_ = 0;
            metro_phase_ = 0.f;
            had_grid_ = false;
        }
        bool Metronome() const { return metro_on_; }
        // the beep is an aid for the first loop, the grid it rides on outlives it
        void SetBeep(bool on) { beep_on_ = on; }
        float SampleRate() const { return sr_; }
        float WheelQuarter() const { return 60.f * sr_ / tempo_; }

        // /8 signatures count their eighths in threes: 6/8 is two dotted-quarter beats
        float QuartersPerBeat() const { return TicksPerBeat() / static_cast<float>(kTicksPerQuarter); }

        // false without a grid
        bool Quarter(float &samples, bool midi_only = false) const
        {
            uint32_t beat;
            float phase;
            bool midi;
            if(!Get(beat, phase, &samples, &midi) || (midi_only && !midi))
                return false;
            samples /= QuartersPerBeat();
            return true;
        }

        // delay sync state, here because the engine's SRAM is full; note -1 is free time
        int DelayNote() const { return delay_note_; }
        void SetDelayNote(int note)
        {
            delay_note_ = note;
            __DMB(); // before the caller writes the delay time
        }
        float &DelayQuarter() { return delay_quarter_; }
        bool DelayTouched() const { return delay_touched_; }
        void TouchDelay() { delay_touched_ = true; }

        int BeatsPerBar() const { return Sig().compound() ? Sig().top / 3 : Sig().top; }

        // the next time signature, the metronome restarts on the one
        void NextBar()
        {
            sig_ = (sig_ + 1) % kSigs;
            SetMetronome(true);
        }

        void ChangeTempo(int bpm)
        {
            bpm += tempo_;
            tempo_ = bpm < kMinTempo ? kMinTempo : bpm > kMaxTempo ? kMaxTempo : bpm;
        }

        // Beats since the start, the phase within the current one and samples per beat, false without a grid.
        // midi tells the two grids apart: they count from their own starts, so their beats are not the same beats.
        bool Get(uint32_t &beat, float &phase, float *length = nullptr, bool *midi = nullptr) const
        {
            uint32_t period;
            if(MidiGrid(beat, phase, period))
            {
                if(length)
                    *length = float(period) * TicksPerBeat() * sr_ / daisy::System::GetTickFreq();
                if(midi)
                    *midi = true;
                return true;
            }
            if(!metro_on_)
                return false;
            beat = metro_beat_;
            phase = metro_phase_;
            if(length)
                *length = BeatSamples();
            if(midi)
                *midi = false;
            return true;
        }

        // once per audio block: runs the metronome and adds its beep to all outputs
        void Process(float **out, size_t size, float hp_gain, float line_gain)
        {
            if(metro_on_)
            {
                metro_phase_ += size / BeatSamples();
                while(metro_phase_ >= 1.f)
                {
                    metro_phase_ -= 1.f;
                    metro_beat_++;
                }
            }

            // a beep on every beat, a higher one on the one; 6/8 also clicks the eighths between, quieter
            uint32_t beat;
            float phase;
            const bool grid = metro_on_ && beep_on_ && Get(beat, phase);
            const uint32_t subs = Sig().compound() ? 3 : 1;
            const uint32_t sub = static_cast<uint32_t>(phase * subs);
            const uint32_t pulse = beat * subs + sub;
            if(grid && (!had_grid_ || pulse != last_pulse_) && phase * subs - sub < .25f)
            {
                beep_env_ = sub ? kBeepLevel * .5f : kBeepLevel;
                beep_ph_ = 0.f;
                beep_inc_ = (sub ? 800.f : beat % BeatsPerBar() ? 1000.f : 1500.f) / sr_;
            }
            had_grid_ = grid;
            if(grid)
                last_pulse_ = pulse;

            for(size_t i = 0; i < size && beep_env_ > .001f; i++)
            {
                const float b = sinf(beep_ph_ * 6.2831853f) * beep_env_;
                out[0][i] += b * hp_gain;
                out[1][i] += b * hp_gain;
                out[2][i] += b * line_gain;
                out[3][i] += b * line_gain;
                beep_ph_ += beep_inc_;
                if(beep_ph_ >= 1.f)
                    beep_ph_ -= 1.f;
                beep_env_ *= beep_decay_;
            }
        }

    private:
        bool MidiGrid(uint32_t &beat, float &phase, uint32_t &period) const
        {
            daisy::ScopedIrqBlocker block;
            const int32_t since = daisy::System::GetTick() - t_;
            period = period_;
            if(!running_ || !period_ || !count_ || since > int32_t(2 * period_))
                return false;

            // runs on past a late tick instead of waiting for it
            const uint32_t tick = count_ - 1;
            const uint32_t per_beat = TicksPerBeat();
            float into = tick % per_beat + (since > 0 ? float(since) / period_ : 0.f);
            beat = tick / per_beat;
            if(into >= per_beat)
            {
                into -= per_beat;
                beat++;
            }
            phase = into / per_beat;
            return true;
        }

        struct Signature
        {
            uint8_t top, bottom;
            bool compound() const { return bottom == 8; }
        };
        static constexpr uint8_t kSigs = 5;
        // the order the wheel steps through, most used first
        const Signature& Sig() const
        {
            static const Signature sigs[kSigs] = {{4, 4}, {3, 4}, {6, 8}, {5, 4}, {7, 4}};
            return sigs[sig_];
        }
        uint32_t TicksPerBeat() const { return Sig().compound() ? kTicksPerQuarter * 3 / 2 : kTicksPerQuarter; }

        // the tempo counts quarters, a 6/8 beat lasts one and a half
        float BeatSamples() const { return 60.f * sr_ / tempo_ * TicksPerBeat() / kTicksPerQuarter; }

        static bool IsTransport(uint8_t byte)
        {
            return byte == 0xF8 || byte == 0xFA || byte == 0xFB || byte == 0xFC || byte == 0xF2;
        }

        static uint32_t Lost() { return daisy::System::GetTickFreq() / 4; } // slower than 10 BPM

        // times are TIM2 ticks, which wrap at 2^32 (GetUs doesn't); a small PLL keeps USB jitter off the grid
        void Tick(uint32_t now)
        {
            const uint32_t d = now - last_;
            last_ = now;
            if(running_)
                count_++;

            if(d >= Lost())
            {
                period_ = 0;
                t_ = now;
                return;
            }
            if(!period_)
            {
                period_ = d;
                t_ = now;
                outliers_ = 0;
                return;
            }

            const int32_t err = now - (t_ + period_);
            t_ += period_;
            if(err > int32_t(period_ / 2) || -err > int32_t(period_ / 2))
            {
                // bunched USB packets or a lost byte keep the grid, a real tempo jump resyncs
                if(++outliers_ >= 3)
                {
                    period_ = d;
                    t_ = now;
                    outliers_ = 0;
                }
                return;
            }

            outliers_ = 0;
            t_ += err / 4;
            period_ += err / 16;
        }

        const void *source_; // the handler the clock comes from
        bool started_;       // the count is the song position
        bool running_;       // between start / continue and stop
        uint8_t spp_;        // song position byte to come: 1 = LSB, 2 = MSB, 0 = none
        uint8_t spp_lsb_;
        uint32_t count_;     // ticks since start
        uint32_t outliers_;  // ticks in a row far off the grid
        uint32_t heard_;     // last byte from source_
        uint32_t last_;      // last tick, raw
        uint32_t t_;         // last tick, smoothed
        uint32_t period_;    // smoothed tick length, 0 while unknown

        bool metro_on_;
        bool beep_on_;      // the beep, off once the first loop is down
        bool had_grid_;      // a grid that just appeared beeps at once
        int tempo_;          // BPM in quarters
        uint8_t sig_;        // time signature, see Sig()
        volatile int8_t delay_note_;
        float delay_quarter_;
        bool delay_touched_;
        uint32_t metro_beat_;
        uint32_t last_pulse_;
        float metro_phase_;
        float sr_;
        float beep_env_, beep_ph_, beep_inc_, beep_decay_;
    };

    extern Clock beat_clock;
}
