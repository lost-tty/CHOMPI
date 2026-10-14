#pragma once

#include <stdint.h>
#include <atomic>
#include "daisysp.h"
#include "RamBuffer.h"
#include "Stream.h"

namespace chompi
{
    // Plays a sample from RAM or from its stream, cubic between frames, with one short fade for every jump.
    // Audio thread; the setters are called from the UI and the engine.
    class Voice
    {
      public:
        static constexpr uint32_t kNoFrame = ~0u;

        void Init(Stream *stream, float sr, daisy::RamBufferMemory *ram);

        // adds n samples
        void Render(float *l, float *r, size_t n);
        // every block, the voice playing or not: keeps the stream's next note filled from the start
        void Prepare();

        void StartPlaying(uint32_t from = kNoFrame); // from: a frame to begin at this once, else the start
        void StopPlaying();
        void Choke();
        bool IsPlaying() const { return env_.IsRunning() || gate_; }
        bool Active() const { return IsPlaying() || pending_ != Action::kNone || waiting_; }
        bool IsHeld() const { return gate_; }
        float Level() const { return level_ * velocity_ * gain_; } // without pan

        // the source: RAM, the recording or the loaded slot at its own rate, or the stream of a file
        void SetRamSource(daisy::RamBufferMemory *ram, bool loaded);
        void SetUsingRam(bool use, bool click);
        void OpenFile(const char *name, bool reset);
        void CloseFile();
        bool IsLoaded() const { return loaded_; }
        bool UsesRam() const { return use_ram_target_; }
        const daisy::RamBufferMemory *RamSource() const { return ram_; }
        bool IsKit() const;

        // start and end, 0..1; false when too close for a loop
        bool SetStartPoint(float val);
        bool SetEndPoint(float val);
        void SetStartPointForce(float val);
        void SetEndPointForce(float val);
        float StartPoint() const { return start_; }
        float EndPoint() const { return end_; }
        uint32_t Frames() const;
        void SetReverse(bool rev);
        bool GetReverse() const { return reverse_; }
        void SetAutoLoop(bool loop);
        void ToggleAutoLoop() { SetAutoLoop(!auto_loop_); }
        bool GetAutoLoop() const { return auto_loop_; }
        bool Loops() const { return auto_loop_ && !IsKit(); } // a kit slice plays once

        // speed: for the next note, while one plays
        void SetVarispeed(float speed);
        // Legato: the note under way goes to speed over the glide.
        void Retune(float speed)
        {
            next_speed_ = speed;
            gliding_ = true;
        }
        void SetGlide(float seconds) { glide_rate_ = seconds > 0.f ? 1.f / (seconds * sr_) : 0.f; }
        void SetGlobalPitch(float speed) { pitch_ = speed; }
        float GetGlobalPitch() const { return pitch_; }

        void SetVelocity(float vel) { velocity_ = vel * 0.00787401f; }
        void SetGain(float val);
        void SetPan(float val);
        float GetPan() const { return pan_; }
        void SetAttack(float val);
        void SetDecay(float val);
        void SetSustainActive(bool sus) { sustain_ = sus; }
        bool GetSustainActive() const { return sustain_; }
        void ToggleSustainActive() { sustain_ = !sustain_; }
        void RestoreDefaults();

        // which CUBBI slot the voice was set up for
        void SetCubbiMode(bool mode) { cubbi_mode_ = mode; }
        bool GetCubbiMode() const { return cubbi_mode_; }
        void SetBank(uint8_t b) { bank_ = b; }
        uint8_t GetBank() const { return bank_; }
        void SetSlot(uint8_t s) { slot_ = s; }
        uint8_t GetSlot() const { return slot_; }
        void SetCopyOccurred() { copy_occurred_ = true; }
        bool GetCopyOccurred()
        {
            const bool ret = copy_occurred_;
            copy_occurred_ = false;
            return ret;
        }

      private:
        // what the fade waits for: at its bottom it happens, then the fade comes up again
        enum class Action : uint8_t { kNone, kJump, kFlip, kRestart, kStop };
        // what the ring that doesn't play is filled for
        enum class Spare : uint8_t { kNone, kStart, kHere };

        void Sample(float &l, float &r);
        void Block();
        void Snap();
        void Sync();
        bool Ready(Action a);
        void Request(Action a);
        void Begin(bool trigger);
        void Do(Action a);
        void Stop();
        bool Next(float &l, float &r);
        uint32_t Skip(uint32_t n);
        bool Prime();
        float Ahead() const;
        Span RamSpan() const;
        Ring &Cur() const { return stream_->ring[cur_]; }
        void Fill(Spare what, uint32_t pos = kNoFrame);
        void Swap();
        void Wait();
        void Reverse();
        bool Fits(float start, float end) const;
        bool SpareReady() const;
        void Publish();
        void Invalidate();

        Stream *stream_;
        daisy::RamBufferMemory *ram_;
        bool use_ram_, use_ram_target_, loaded_;
        uint32_t open_seq_;
        int cur_;           // the ring that plays
        uint32_t seq_[2];   // the rings' restarts
        Spare spare_;
        uint32_t spare_pos_; // says which frame the spare was filled from; kNoFrame means the span's first
        bool waiting_, running_;
        uint32_t waited_;   // samples a jump waits for the spare ring

        Span span_;         // RAM: this block's
        float inc_;         // this block's frames per sample
        float inv_fade_;    // 1 / frames a jump fades over
        bool ring_ok_;      // stream: the ring is filled for this restart, written and done as of this block
        uint32_t avail_, done_;
        uint32_t pos_;      // RAM: the next frame
        bool gap_;          // RAM: the next frame is after a jump
        float hist_[4][2];  // the frames around the position, cubic between [1] and [2]
        float phase_;
        int jump_;            // hist_ index of the first frame after the last jump
        float dip_;           // the gain around jumps
        bool ended_;

        // stream: where it reads the ring, backwards through what it played after a flip, down to floor_
        uint32_t idx_;
        bool back_;
        uint32_t floor_;
        bool floor_ends_;     // the note ends at floor_, no spare goes on from there
        uint32_t next_jump_;  // the ring frame where it jumps next: forward the first after it, backwards the last before it

        float start_, end_;
        uint32_t from_;
        bool reverse_, play_reverse_, auto_loop_;
        float speed_, next_speed_, pitch_, glide_rate_;
        bool gliding_;
        float sr_;

        std::atomic<Action> pending_; // set from the UI and the audio
        bool trigger_;
        float fade_;

        daisysp::Adsr env_;
        bool gate_, sustain_;
        float level_;
        float attack_, release_;
        float velocity_, gain_, gain_target_;
        float pan_, pan_l_, pan_l_target_, pan_r_, pan_r_target_;

        bool cubbi_mode_, copy_occurred_;
        uint8_t bank_, slot_;
    };
}
