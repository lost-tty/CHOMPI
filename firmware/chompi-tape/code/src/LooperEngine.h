/** DSPEngine
 *  Core DSP for sampling engine, looping engine, and additional DSP
 */
#pragma once
#include "Sampler.h"
#include "RamBuffer.h"
#include "Clock.h"
using namespace daisy;

static const uint32_t kRecordClearTimeout = 2000;
static const uint32_t kButtonTimeout = 10;
namespace daisy
{
    // the first loop on the beat grid, in DTCM because the SRAM is full
    struct LoopSync
    {
        void Init()
        {
            filling = synced = stop_play = loading = play_wait = play_armed = trigger_req = false;
            midi_grid = play_midi = false;
            play_press = play_at = trigger_press = 0;
            fill_from = start = start_at = stop_at = 0;
            pre_frames = 0;
            quarter = 0.f;
            lead = tail = Fold();
        }

        bool filling = false;          // the buffer holds the input since beat fill_from
        bool synced = false;           // the recording started on beat start
        bool stop_play = false;        // the waiting stop goes to play, not overdub
        volatile bool loading = false; // the copier writes the buffer
        volatile bool play_wait = false; // play was pressed at play_press (ms), the audio callback places it
        bool play_armed = false;         // playback starts on beat play_at
        bool midi_grid = false;          // the recording's grid is the MIDI one, not the metronome's
        bool play_midi = false;          // the same for a waiting playback start
        volatile bool trigger_req = false; // a key triggered the armed recording at trigger_press (ms)
        uint32_t play_press = 0, play_at = 0, trigger_press = 0;
        uint32_t fill_from = 0, start = 0;
        uint32_t start_at = 0, stop_at = 0; // beats of a waiting start or stop, 0 = none
        uint32_t pre_frames = 0;           // input frames between an early press and the start beat
        float quarter = 0.f;               // samples per quarter of a snapped loop, 0 = unknown
        // audio folded across the loop point a chunk per block, up to half a beat is too long for one
        struct Fold
        {
            size_t from = 0, to = 0, done = 0, total = 0;
            bool fade_out = false;
        } lead, tail;
    };
    extern LoopSync loop_sync;

    /** @brief Logic for main looper engine */
    class LooperEngine
    {
    public:
        LooperEngine() {}
        ~LooperEngine() {}

        void Init(float sr, RamBufferMemory* loop_buff)
        {
            looper.Init(sr, loop_buff);

            record = false;
            first_record = true;
            record_target = false;
            record_arm = false;
            playing = false;

            char name_buffer[32];
            sprintf(name_buffer, "looper.wav");
            looper.Reset();

            fx_env_ = fx_env_target_ = 1.f;
        }

        void FXEnvelope()
        {
            if(IsFirstRecording() && !IsRecording() && !looper.IsResetting())
                return;

            fx_env_target_ = 0.f;
        }

        void Process(float* out_l, float* out_r, size_t size)
        {
            OverlayStep();

            if(first_record && !looper.IsResetting())
            {
                if(SyncBlock(out_l, out_r, size))
                    return;
            }
            else
            {
                loop_sync.filling = false;
                if(!first_record)
                    PlayBlock();
            }

            // looper.PerBlock(playing);

            const bool undoing = looper.ram_buff.IsUndoing();
            if(length_req)
            {
                if(!first_record && !IsPrinting() && !undoing)
                    looper.ChangeLength(length_req);
                length_req = 0;
            }
            if(step_req)
            {
                if(!first_record && !IsPrinting() && !undoing)
                    looper.StepLoop(step_req);
                step_req = 0;
            }
            if(undo_req)
            {
                if(CanUndo() && looper.Undo())
                    record = record_target = false;
                undo_req = false;
            }
            if(mark_req_ && !record) // marks the recording as written, its last block included
            {
                mark_req_ = false;
                looper.ram_buff.MarkRecorded();
            }
            // A copy or a load addresses the loop as it is, so the cut waits.
            if(cut_req_ && !looper.ram_buff.IsCopying() && !looper.ram_buff.IsPending())
            {
                cut_req_ = false;
                if(!first_record && !IsPrinting() && !undoing)
                {
                    looper.Cut();
                    uint32_t beat;
                    float phase, len;
                    bool midi;
                    if(chompi::beat_clock.Get(beat, phase, &len, &midi)) // cut to the clock: the loop has its tempo
                        loop_sync.quarter = len / chompi::beat_clock.QuartersPerBeat();
                }
            }
            if(!playing)
                looper.ApplyPendingNow();
            if(base_req && !looper.ram_buff.IsPending()) // waits for a pending change
            {
                if(!first_record)
                    looper.ram_buff.MakeLengthBase();
                base_req = false;
            }
            if(looper.ram_buff.IsCopying())
                looper.ram_buff.CopyStep();
            if(looper.ram_buff.IsUndoing())
                looper.ram_buff.UndoStep();

            if(fx_env_ < .01f)
                fx_env_target_ = 1.f;

            for(size_t i = 0; i < size; i++)
            {
                const bool print = looper.IsPrinting();
                daisysp::fonepole(fx_env_, fx_env_target_, .001f);

                if(!first_record || looper.IsResetting())
                {
                    int16_t aol = 0;
                    int16_t aor = 0;

                    const int16_t inl = f2s16(print ? print_l_[i] : out_l[i] * fx_env_);
                    const int16_t inr = f2s16(print ? print_r_[i] : out_r[i] * fx_env_);
                    // an overdub waits for an undo to finish
                    looper.PopStereoSamps(inl, inr, &aol, &aor, record && !looper.ram_buff.IsUndoing(), playing);

                    out_l[i] += s162f(aol) * fx_env_;
                    out_r[i] += s162f(aor) * fx_env_;
                }
                else if(!reset)
                {
                    looper.PushStereoSamps(f2s16(out_l[i] * fx_env_), f2s16(out_r[i] * fx_env_));
                }

                if(looper.WriteFullLength())
                {
                    loop_sync.stop_at = loop_sync.pre_frames = 0; // no loop point to wrap around
                    ToggleRecord(); // overdub mode
                    ToggleRecord(); // play mode
                }
            }
        }

        bool StartPrint()
        {
            if(first_record || !SteadyPlayback() || IsPrinting() || looper.ram_buff.IsCopying()
               || looper.ram_buff.IsPending() || looper.ram_buff.IsUndoing())
                return false;

            print_state_ = PrintState::STARTING;
            return true;
        }

        inline bool SteadyPlayback()
        {
            return playing && fabsf(looper.GetScrub() - looper.GetVarispeed()) <= .01f;
        }

        bool CapturePrint(const float* l, const float* r, size_t size)
        {
            if(!IsPrinting())
                return false;

            if(!SteadyPlayback() || (print_state_ == PrintState::PRINTING && !looper.IsPrinting()))
            {
                looper.StopPrint();
                print_state_ = PrintState::OFF;
                return false;
            }

            size = size < kMaxPrintBlock ? size : kMaxPrintBlock; // audio blocks are 24
            std::copy(l, l + size, print_l_);
            std::copy(r, r + size, print_r_);

            if(print_state_ == PrintState::STARTING)
            {
                looper.StartPrint(static_cast<int32_t>(fabsf(looper.GetScrub()) * size) * 2);
                print_state_ = PrintState::PRINTING;
            }
            else if(print_state_ == PrintState::PRINTING && looper.PrintPassDone())
            {
                print_state_ = PrintState::OFF;
                return true;
            }
            return false;
        }

        inline bool IsPrinting() { return print_state_ != PrintState::OFF || looper.IsPrinting(); }

        inline bool IsPlaying() const { return playing && !first_record; }
        inline bool IsFirstRecording() const { return first_record; }

        void IncrementDubGain(float gain) { looper.IncrementDubGain(gain, first_record && !record); }
        inline float GetDubGain() { return looper.GetDubGain(); }

        bool CheckReset()
        {
            if(!looper.IsResetting() && !record_btn && !play_btn)
                reset = false;

            uint32_t elapsed = System::GetNow() - last_button_press;            
            if(elapsed >= kButtonTimeout)
            {
                if(play_btn && !record_btn && !reset && !toggle_play && (record || IsPlaying())) // rising edge and recording or playing
                    TogglePlaying();
                if(record_btn && !play_btn && !reset && !toggle_record && !ToggleRecordOnGrid(false, last_button_press))
                    ToggleRecord();
            }
            if(elapsed >= kRecordClearTimeout)
            {
                if(!record_btn && play_btn && !jump_to_start && !IsPlaying())
                {
                    looper.JumpToStart();
                    jump_to_start = true;
                    return false;
                }
                else if(record_btn && play_btn && !reset)
                {
                    Reset();
                    return true;
                }
            }

            return false;
        }

        void CheckRecordReady()
        {
            if(loop_sync.trigger_req) // here, so only this thread touches the grid state
            {
                const uint32_t press = loop_sync.trigger_press;
                loop_sync.trigger_req = false;
                if(record_arm)
                {
                    if(!ToggleRecordOnGrid(false, press))
                        ToggleRecord();
                }
            }

            if(record_target)
            {
                record = true;
                record_target = false;
                looper.ReadyToRecord();

                if(IsFirstRecording())
                    looper.ResetRamBuff();
            }

            if(play_target)
            {
                playing = true;
                play_target = false;
            }
        }

        bool IsRecording()
        {
            return record;
        }

        inline void SetPitch(float val) { looper.SetVarispeed(val); }
        inline void SetTapeSlew(bool on) { looper.SetTapeSlew(on); }
        inline void SetReverse(bool rev) { looper.SetReverse(rev); }
        inline void SetScrub(float scrub) { looper.SetScrub(scrub); }
        inline float GetScrub() { return looper.GetScrub(); }
        inline bool GetReverse() { return looper.GetReverse(); }

        void RecordButton(bool rising)
        {

            record_btn = rising;
            toggle_record = false;
            last_button_press = System::GetNow();

            if(looper.IsResetting() || reset || record_arm)
                return; // short circuit situations
            else if(record_btn && play_btn && first_record && !record)
                record_arm = true;
        }

        void PlayButton(bool rising)
        {
            // falling edge and not recording
            if(play_btn && !rising && !jump_to_start && !record && !toggle_play)
                TogglePlaying();

            toggle_play = false;

            jump_to_start = false;

            play_btn = rising;
            last_button_press = System::GetNow();
         
            if(looper.IsResetting() || reset || record_arm)
                return; // short circuit situations
            else if(record_btn && play_btn && first_record && !record)
                record_arm = true;
        }

        void ToggleRecord(bool play_pressed = false)
        {
            toggle_record = true;
            record_arm = false;
            if(record)
            {
                // don't go into overdub from new record if play is pressed
                if(play_pressed || !first_record)
                {
                    record_target = record = false;
                }

                if(first_record) // reload, then overdub
                {
                    mark_req_ = true;
                    looper.JumpToStart();
                }

                first_record = false;
                play_target = true;
                playing = false;
            }
            else
            {
                if(first_record)
                {
                    looper.WriteJump(sizeof(WAV_FormatTypeDef));
                }
                if (!first_record)
                {
                    playing = true;
                }
                record_target = true;
            }
        }

        // a key triggers the armed recording, the audio thread runs it on the grid
        void TriggerRecord()
        {
            loop_sync.trigger_press = System::GetNow();
            __DMB(); // the audio thread must not see the request before the time
            loop_sync.trigger_req = true;
        }

        void TogglePlaying()
        {
            toggle_play = true;

            if(first_record && !record)
                return; // do nothing on empty looper and not recording

            if(record)
            {
                if(!ToggleRecordOnGrid(true, last_button_press))
                    ToggleRecord(true);
            }
            else if(playing || !PlayOnGrid())
            {
                playing = !playing;
            }
        }

        // In loop edit the points are samples of the recording. With a clock, END keeps the loop a whole number
        // of 16ths from the start.
        float Point(int k) const { return looper.ram_buff.CutFraction(k); }

        bool MoveCut(int k, int turns, uint32_t step) // step says how many frames a detent moves
        {
            RamBuffer &rb = looper.ram_buff;
            int32_t n = turns * int32_t(step) * 2;
            uint32_t beat;
            float phase, len;
            bool midi;
            if(k == 1 && chompi::beat_clock.Get(beat, phase, &len, &midi))
            {
                const int32_t sixteenth = int32_t(len / chompi::beat_clock.QuartersPerBeat() / 4.f) * 2;
                const int32_t count = (int32_t(rb.CutLen()) + sixteenth / 2) / sixteenth + turns;
                if(sixteenth <= 0 || count < 1)
                    return false;
                n = count * sixteenth - int32_t(rb.CutLen());
            }
            if(!rb.MoveCut(k, n))
                return false;
            cut_req_ = true;
            return true;
        }

        bool SetCut(int k, size_t pos)
        {
            if(!looper.ram_buff.SetCut(k, pos))
                return false;
            cut_req_ = true;
            return true;
        }

        size_t Cut(int k) const { return looper.ram_buff.Cut(k); }

        daisy::RamBufferMemory *Memory() const { return looper.ram_buff.Memory(); }
        size_t RecStart() const { return looper.ram_buff.RecStart(); }
        size_t RecLen() const { return looper.ram_buff.RecLen(); }

        // Returns the playhead as a fraction of the recording.
        float Playhead() const
        {
            const RamBuffer &rb = looper.ram_buff;
            return rb.RecLen() ? float(double(rb.GetReadHead() - rb.RecStart()) / rb.RecLen()) : 0.f;
        }

        void RequestLengthChange(int8_t steps)
        {
            if(!first_record && !length_req && !IsPrinting())
                length_req = steps;
        }

        inline int8_t GetLengthExp() { return looper.ram_buff.LengthExp(); }

        void RequestStep(int8_t dir)
        {
            if(!first_record && !step_req && !IsPrinting())
                step_req = dir;
        }

        inline size_t GetSection() { return looper.ram_buff.Section(); }

        bool RequestUndo()
        {
            if(!CanUndo())
                return false;
            undo_req = true;
            return true;
        }

        inline bool CanUndo()
        {
            return !first_record && !IsPrinting() && looper.ram_buff.HasUndo()
                   && !looper.ram_buff.IsUndoing() && !looper.ram_buff.IsCopying();
        }

        inline void RequestLengthBase()
        {
            if(!first_record)
                base_req = true;
        }

        inline bool GetRecordArm() { return record_arm; }
        inline bool GetIsEmpty() { return !record && first_record; }
        
        // the playhead in quarters from the loop start, for a loop with a tempo; the tell counts L and R
        float GetQuarters() { return looper.GetReadTell() * .5f / loop_sync.quarter; }

        float GetPosition() { 
            float tell = static_cast<float>(looper.GetReadTell());
            float size = static_cast<float>(looper.GetReadSize());
            return tell / size;
        }

        float GetPitch()
        {
            return looper.GetVarispeed();
        }

        inline bool GetReset() { return reset; }

        void Reset()
        {
            looper.Reset();
            reset = true;
            const bool loading = loop_sync.loading; // the copier goes on until OpenFile
            loop_sync.Init();
            loop_sync.loading = loading;

            looper.StopPrint();
            print_state_ = PrintState::OFF;

            record = false;
            first_record = true;
            record_target = false;
            record_arm = false;
            playing = false;

            // an empty looper is where the metronome is switched, so clearing leaves it off and one
            // click starts it. Its grid outlives the first loop, it must not outlive the loop itself.
            chompi::beat_clock.SetMetronome(false);
            chompi::beat_clock.SetDelayNote(-1);
        }

        // while a loop is loaded over this one: that also drops a playback waiting for the beat
        inline void Stop(bool play)
        {
            playing = play;
            record = record_target = false;
            loop_sync.play_wait = loop_sync.play_armed = false;
            loop_sync.quarter = 0.f;
        }

        void OpenFile(bool play)
        {
            loop_sync.Init();
            first_record = false;
            playing = play;
            record = false;
            record_arm = false;
            record_target = false;
            looper.ram_buff.MarkRecorded();

            looper.SetVarispeed(1.f, true);
            looper.JumpToStart();
            looper.ForceSetScrub(0.f);
        }

    private:
        // Empty with a grid, the input fills the buffer from the last beat, so a start just after it can
        // begin on it. Also runs waiting starts and stops. True when there is nothing else to do.
        bool SyncBlock(float* out_l, float* out_r, size_t size)
        {
            LoopSync& s = loop_sync;
            uint32_t beat = 0;
            float phase = 0.f;
            float len = 0.f;
            bool midi = false;
            const bool grid = chompi::beat_clock.Get(beat, phase, &len, &midi);

            if(record)
            {
                // beats past the stop, relative: a float of the whole count loses precision
                const float past = static_cast<int32_t>(beat - s.stop_at) + phase;
                // gone, swapped for the other grid, or went back: stop as is
                const bool lost = !grid || midi != s.midi_grid || beat < s.start;
                if(s.stop_at && (lost || past >= 0.f))
                {
                    if(!lost)
                    {
                        const size_t late = std::min(static_cast<size_t>(past * len), looper.ram_buff.GetSize() / 2);
                        looper.ram_buff.Trim(looper.ram_buff.GetSize() - 2 * late);
                        s.quarter = looper.ram_buff.GetSize() / 2
                                    / ((s.stop_at - s.start) * chompi::beat_clock.QuartersPerBeat());
                        WrapOverlays(late);
                    }
                    s.stop_at = s.pre_frames = 0;
                    ToggleRecord(s.stop_play);
                }
                return false;
            }

            if(s.loading)
            {
                s.filling = false;
                s.start_at = s.pre_frames = 0;
                return true;
            }

            if(!grid)
            {
                s.filling = false;
                if(s.start_at) // the clock went away, start now
                {
                    s.start_at = s.pre_frames = 0;
                    ToggleRecord();
                }
                return true;
            }

            // the fill follows the clock's beat, also when it jumps (start, song position, early tick)
            if(!s.filling || s.fill_from != beat)
            {
                if(phase * len < size) // a beat that began less than a block ago starts one now
                    NextFill(beat);
                else
                    s.filling = false;
            }

            if(s.start_at)
            {
                if(midi != s.midi_grid) // another grid counts its own beats
                {
                    s.midi_grid = midi;
                    s.start_at = beat + 1;
                }
                if(beat + 1 < s.start_at) // the clock went back
                    s.start_at = beat + 1;
                if(s.filling && s.fill_from >= s.start_at)
                {
                    StartOnGrid(s.fill_from);
                    return false;
                }
            }

            const size_t next = static_cast<size_t>((1.f - phase) * len);
            for(size_t i = 0; i < size; i++)
            {
                if(i == next)
                    NextFill(beat + 1);
                daisysp::fonepole(fx_env_, fx_env_target_, .001f);
                looper.PushStereoSamps(f2s16(out_l[i] * fx_env_), f2s16(out_r[i] * fx_env_));
            }
            return true;
        }

        // with a clock, play starts on the beat nearest to the key press
        bool PlayOnGrid()
        {
            uint32_t beat;
            float phase;
            bool midi;
            if(!chompi::beat_clock.Get(beat, phase, nullptr, &midi))
                return false;
            loop_sync.play_midi = midi;
            loop_sync.play_press = last_button_press;
            __DMB();
            loop_sync.play_wait = true;
            return true;
        }

        void PlayBlock()
        {
            LoopSync& s = loop_sync;
            if(playing) // started some other way meanwhile
                s.play_wait = s.play_armed = false;
            if(!s.play_wait && !s.play_armed)
                return;

            uint32_t beat;
            float phase, len;
            bool midi;
            if(!chompi::beat_clock.Get(beat, phase, &len, &midi) || midi != s.play_midi)
            {
                s.play_wait = s.play_armed = false; // no grid, or not the one it was waiting on: now
                playing = true;
                return;
            }

            if(s.play_wait)
            {
                // play starts on release, the press is what counts
                const float pressed = phase - (System::GetNow() - s.play_press) * chompi::beat_clock.SampleRate() * .001f / len;
                const int32_t k = static_cast<int32_t>(floorf(pressed + .5f));
                s.play_at = k < 0 && beat < static_cast<uint32_t>(-k) ? 0 : beat + k;
                s.play_wait = false;
                s.play_armed = true;
            }

            const float past = static_cast<int32_t>(beat - s.play_at) + phase;
            if(past >= 0.f)
            {
                looper.SkipRead(static_cast<int32_t>(past * len * looper.GetVarispeed() + looper.StartLag()));
                s.play_armed = false;
                playing = true;
            }
        }

        // The fill for the next beat goes above the last one, which stays as the lead-in below the loop.
        // A broken run starts again at 0, so all below the loop start is one stretch of input.
        void NextFill(uint32_t beat)
        {
            LoopSync& s = loop_sync;
            const size_t end = looper.ram_buff.End();
            looper.ram_buff.FillFrom(s.filling && end <= kMaxSyncFill ? end : 0);
            s.fill_from = beat;
            s.filling = true;
        }

        // the audio across the loop point belongs on its other side: an early start's lead-in at the
        // end, a late stop's tail at the beginning. Both still sit next to the loop, untrimmed.
        void WrapOverlays(size_t late)
        {
            const size_t len = looper.ram_buff.GetSize(), start = looper.ram_buff.GetStart();
            LoopSync& s = loop_sync;

            size_t n = 2 * late;
            if(n && n <= len)
            {
                s.tail = {start + len, start, 0, n, true};
                FoldStep(s.tail); // playback starts at the loop point in this block
            }

            n = std::min<size_t>(2 * s.pre_frames, start); // only what the fill kept below the loop
            if(n && n <= len)
                s.lead = {start - n, start + len - n, 0, n, false};
        }

        // a chunk per block stays well ahead of playback, which moves a block's worth
        void OverlayStep()
        {
            FoldStep(loop_sync.lead);
            FoldStep(loop_sync.tail);
        }

        void FoldStep(LoopSync::Fold& f)
        {
            if(f.done >= f.total)
                return;
            if(f.to + f.total > looper.ram_buff.End()) // the loop moved under it
            {
                f.done = f.total;
                return;
            }

            const size_t n = std::min<size_t>(f.total - f.done, kCopyStepSize);
            looper.ram_buff.Overlay(f.from, f.to, f.done, n, f.total, f.fade_out);
            f.done += n;
        }

        // the buffer already holds the loop from that beat on
        void StartOnGrid(uint32_t beat)
        {
            loop_sync.start = beat;
            loop_sync.start_at = 0;
            loop_sync.synced = true;
            record = true;
            record_target = false;
            looper.ReadyToRecord();
        }

        // the first loop starts and ends on the beat nearest to the key press
        bool ToggleRecordOnGrid(bool play_pressed, uint32_t press)
        {
            LoopSync& s = loop_sync;
            uint32_t beat;
            float phase, len;
            bool midi;
            if(!first_record || (record && !s.synced) || !chompi::beat_clock.Get(beat, phase, &len, &midi))
                return false;

            toggle_record = true;
            record_arm = false;
            if(s.stop_at) // play while waiting to stop still goes to play
                s.stop_play |= play_pressed;
            if(s.start_at || s.stop_at)
                return true;

            // the toggle comes after the press: kButtonTimeout for a key, a block for a trigger
            const float late = (System::GetNow() - press) * chompi::beat_clock.SampleRate() * .001f / len;
            const float pressed = phase - late; // relative to this beat
            const uint32_t nearest = pressed >= .5f ? beat + 1 : pressed < -.5f && beat ? beat - 1 : beat;

            if(!record)
            {
                s.midi_grid = midi;
                // an early press plays into the loop before it starts, that goes to its end
                s.pre_frames = nearest > beat ? static_cast<uint32_t>((1.f - pressed) * len) : 0;
                if(nearest <= beat && s.filling && s.fill_from == beat)
                    StartOnGrid(beat);
                else
                    s.start_at = beat + 1;
            }
            else
            {
                s.stop_at = nearest > s.start ? nearest : s.start + 1;
                s.stop_play = play_pressed;
            }
            return true;
        }

        FileSampler looper;
        bool record, first_record, record_target;
        bool playing, play_target;
        volatile bool cut_req_ = false, mark_req_ = false;
        uint32_t loop_end;
        uint32_t last_button_press;
        bool record_btn, play_btn;
        bool toggle_record, toggle_play;
        bool record_arm;
        bool reset, jump_to_start;
        volatile int8_t length_req = 0;
        volatile int8_t step_req = 0;
        volatile bool base_req = false;
        volatile bool undo_req = false;

        // PRINTING writes each block a block later
        enum class PrintState { OFF, STARTING, PRINTING };
        PrintState print_state_ = PrintState::OFF;
        static const size_t kMaxPrintBlock = 48;
        float print_l_[kMaxPrintBlock], print_r_[kMaxPrintBlock];

        float fx_env_, fx_env_target_;
    };
}