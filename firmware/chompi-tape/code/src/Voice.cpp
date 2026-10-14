#include "Voice.h"
#include "AiffKit.h"
#include <string.h>
#include <algorithm>

namespace chompi
{
    static constexpr float kFade = 128.f;          // samples, for a jump in or out
    static constexpr uint32_t kMinLoopLen = 4096;  // frames
    static constexpr uint32_t kMask = Ring::kFrames - 1;
    static constexpr int kNoJump = -(1 << 20);
    static constexpr uint32_t kMaxWait = 4800;     // samples a jump waits for the spare ring, then goes silent until it is in

    void Voice::Init(Stream *stream, float sr, daisy::RamBufferMemory *ram)
    {
        stream_ = stream;
        ram_ = ram;
        sr_ = sr;
        use_ram_ = use_ram_target_ = loaded_ = false;
        open_seq_ = 0;
        cur_ = 0;
        seq_[0] = seq_[1] = 0;
        spare_ = Spare::kNone;
        waiting_ = running_ = ended_ = false;
        waited_ = 0;
        span_ = Span();
        inc_ = 1.f;
        pos_ = 0;
        memset(hist_, 0, sizeof(hist_));
        phase_ = 0.f;
        gap_ = false;
        jump_ = kNoJump;
        idx_ = 0;
        back_ = floor_ends_ = false;
        floor_ = 0;
        next_jump_ = ~0u;
        pending_ = Action::kNone;
        trigger_ = false;
        fade_ = 0.f;
        dip_ = 1.f;
        env_.Init(sr);
        env_.SetSustainLevel(1.f);
        gate_ = false;
        level_ = 0.f;
        velocity_ = 1.f;
        cubbi_mode_ = copy_occurred_ = false;
        bank_ = 0;
        slot_ = 15;
        RestoreDefaults();
    }

    void Voice::RestoreDefaults()
    {
        auto_loop_ = sustain_ = true;
        from_ = ~0u;
        pitch_ = 1.f;
        speed_ = next_speed_ = 1.f;
        glide_rate_ = 0.f;
        gliding_ = false;
        reverse_ = false;
        if(!Active())
            play_reverse_ = false;
        start_ = 0.f;
        end_ = 1.f;
        SetAttack(0.f);
        SetDecay(.01f);
        gain_ = gain_target_ = .704f;
        pan_ = .5f;
        pan_l_ = pan_l_target_ = pan_r_ = pan_r_target_ = 1.f;
        spare_ = Spare::kNone;
        Publish();
    }

    void Voice::Render(float *l, float *r, size_t n)
    {
        if(gliding_)
        {
            speed_ += (next_speed_ - speed_) * (glide_rate_ > 0.f ? std::min(1.f, n * glide_rate_) : 1.f);
            if(fabsf(next_speed_ - speed_) < 1e-4f)
            {
                speed_ = next_speed_;
                gliding_ = false;
            }
        }
        Block();
        if(running_ && !use_ram_)
            stream_->speed = inc_;
        for(size_t i = 0; i < n; i++)
        {
            float a, b;
            Sample(a, b);
            l[i] += a;
            r[i] += b;
        }
        Sync();
    }

    // what stays the same over a block, or until a jump changes it
    void Voice::Block()
    {
        inc_ = speed_ * pitch_ * (use_ram_ && loaded_ ? slot_info.rate / sr_ : 1.f);
        inv_fade_ = 1.f / (std::max(inc_, .001f) * kFade);
        span_ = RamSpan();
        Snap();
    }

    // the ring as the streamer left it: read once, not for every frame
    void Voice::Snap()
    {
        if(use_ram_)
            return;
        const Ring &g = Cur();
        ring_ok_ = g.ready_seq == seq_[cur_];
        done_ = g.done;
        avail_ = g.written;
        __DMB(); // the frames before written
    }

    // tells the streamer how far it read
    void Voice::Sync()
    {
        if(use_ram_ || !ring_ok_ || back_) // backwards it reads what it read before
            return;
        Ring &g = Cur();
        if(idx_ > g.read)
            g.read = idx_;
    }

    void Voice::Sample(float &l, float &r)
    {
        const float e = env_.Process(gate_);
        level_ = e;
        if(!sustain_ && env_.GetCurrentSegment() == daisysp::ADSR_SEG_DECAY)
            gate_ = false;

        daisysp::fonepole(gain_, gain_target_, .001f);
        daisysp::fonepole(pan_l_, pan_l_target_, .001f);
        daisysp::fonepole(pan_r_, pan_r_target_, .001f);

        Action p = pending_.load(std::memory_order_relaxed);
        if(p == Action::kFlip) // turns where it is, like tape
        {
            if(pending_.compare_exchange_strong(p, Action::kNone) && reverse_ != play_reverse_)
                Do(Action::kFlip);
        }
        else if(p != Action::kNone)
        {
            if(Ready(p))
            {
                fade_ -= 1.f / kFade;
                if(fade_ <= 0.f || !running_ || (e < .001f && !gate_))
                {
                    fade_ = 0.f;
                    waited_ = 0;
                    Do(pending_.exchange(Action::kNone));
                    Block();
                }
            }
        }
        else if(fade_ < 1.f && running_)
            fade_ = std::min(1.f, fade_ + 1.f / kFade);

        if(waiting_ && Prime())
        {
            waiting_ = false;
            running_ = true;
        }
        if(!running_ || !IsPlaying())
        {
            l = r = 0.f;
            return;
        }

        // frames to move on; of many, only the last four are read
        const float ahead = phase_ + inc_;
        uint32_t steps = static_cast<uint32_t>(ahead);
        phase_ = ahead - steps;
        if(steps > 4)
            steps -= Skip(steps - 4);
        for(; steps; steps--)
        {
            for(int k = 0; k < 3; k++)
            {
                hist_[k][0] = hist_[k + 1][0];
                hist_[k][1] = hist_[k + 1][1];
            }
            if(jump_ > kNoJump)
                jump_--;
            if(!Next(hist_[3][0], hist_[3][1]))
            {
                if(ended_)
                {
                    Stop();
                    l = r = 0.f;
                    return;
                }
                hist_[3][0] = hist_[2][0]; // ran dry: holds until the stream catches up
                hist_[3][1] = hist_[2][1];
            }
        }

        // cubic between [1] and [2]
        const float t = phase_;
        float out[2];
        for(int c = 0; c < 2; c++)
        {
            const float y0 = hist_[0][c], y1 = hist_[1][c], y2 = hist_[2][c], y3 = hist_[3][c];
            const float c1 = .5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.f * y2 - .5f * y3;
            const float c3 = .5f * (y3 - y0) + 1.5f * (y1 - y2);
            out[c] = ((c3 * t + c2) * t + c1) * t + y1;
        }

        // silent while a jump is among the four frames, a fade down to it and up from it
        const float out_of = (4.f + Ahead() - 3.f - t) * inv_fade_, into = (t - jump_) * inv_fade_;
        dip_ = std::min(daisysp::fclamp(std::min(out_of, into), 0.f, 1.f), dip_ + 1.f / kFade); // a turn mid-dip comes up gently
        const float k = e * fade_ * dip_ * velocity_ * gain_;
        l = out[0] * k * pan_l_;
        r = out[1] * k * pan_r_;
    }

    // whether the fade may start: a note on the stream waits, the old one playing on, until the spare ring is filled for it
    bool Voice::Ready(Action a)
    {
        if(a != Action::kRestart || use_ram_target_ || !running_)
            return true;
        if(spare_ != Spare::kStart || spare_pos_ != from_)
            Fill(Spare::kStart, from_);
        stream_->ring[cur_ ^ 1].prefill = false;
        return SpareReady() || ++waited_ > kMaxWait;
    }

    // a new note: from the first frame of the span, the env again if trigger
    void Voice::Begin(bool trigger)
    {
        use_ram_ = use_ram_target_;
        play_reverse_ = reverse_;
        ended_ = false;
        fade_ = 0.f;
        if(use_ram_)
        {
            span_ = RamSpan();
            pos_ = span_.Contains(from_) ? from_ : span_.First();
            gap_ = false;
            running_ = Prime();
            waiting_ = false;
        }
        else
        {
            if(spare_ != Spare::kStart || spare_pos_ != from_)
                Fill(Spare::kStart, from_);
            Wait();
        }
        from_ = ~0u;
        if(trigger)
        {
            env_.Retrigger(false);
            gate_ = true;
        }
    }

    // note on and stop replace what waits; a jump or flip only waits when nothing else does
    void Voice::Request(Action a)
    {
        if(a == Action::kRestart || a == Action::kStop)
        {
            pending_ = a;
            return;
        }
        Action none = Action::kNone;
        pending_.compare_exchange_strong(none, a);
    }

    void Voice::Do(Action a)
    {
        switch(a)
        {
            case Action::kJump: // the points moved past it
                pos_ = span_.First();
                gap_ = true;
                break;
            case Action::kRestart:
                speed_ = next_speed_;
                Begin(trigger_);
                break;
            case Action::kFlip: Reverse(); break;
            case Action::kStop: Stop(); break;
            case Action::kNone: break;
        }
    }

    // the spare ring plays, from its first frame
    void Voice::Swap()
    {
        cur_ ^= 1;
        Cur().prefill = false;
        spare_ = Spare::kNone;
        idx_ = 0;
        back_ = false;
        next_jump_ = ~0u;
        Snap();
    }

    // the spare ring plays once it is filled, after a fade
    void Voice::Wait()
    {
        Swap();
        fade_ = 0.f;
        running_ = false;
        waiting_ = true;
    }

    // the other way from where it is: the four frames in reverse, then on from the one before them
    void Voice::Reverse()
    {
        play_reverse_ = reverse_;
        if(!running_)
        {
            if(waiting_ && !use_ram_) // nothing played yet: from the start the other way
                Begin(false);
            return;
        }
        std::swap(hist_[0], hist_[3]);
        std::swap(hist_[1], hist_[2]);
        phase_ = 1.f - phase_;
        jump_ = jump_ >= 1 && jump_ <= 3 ? 4 - jump_ : kNoJump;

        if(use_ram_)
        {
            span_ = RamSpan();
            uint32_t p = pos_;
            for(int k = 0; k < 5; k++)
                if(!span_.Contains(p) || !span_.Next(p))
                {
                    gap_ = true; // at an edge: on from the top the other way
                    pos_ = span_.First();
                    return;
                }
            pos_ = p;
            gap_ = false;
            return;
        }

        const Ring &g = Cur();
        if(back_) // forward again: on in this ring, it fills on
        {
            idx_ += 5;
            back_ = false;
            spare_ = Spare::kNone;
            next_jump_ = ~0u;
            return;
        }
        Sync();
        const uint32_t floor = g.read > Ring::kHistory ? g.read - Ring::kHistory : 0;
        if(idx_ < floor + 5) // too little played: the spare from here
        {
            Fill(Spare::kHere, idx_ ? g.PosAt(idx_ - 1) : ~0u);
            Wait();
            return;
        }
        idx_ -= 5;
        back_ = true;
        floor_ = floor;
        next_jump_ = g.PrevJump(idx_ + 1);
        // the spare goes on from what played before floor_
        uint32_t p = g.PosAt(floor_);
        const Span span = Span::Of(start_, end_, stream_->frames, play_reverse_, Loops());
        floor_ends_ = p != ~0u && span.Contains(p) && !span.Next(p);
        if(!floor_ends_)
            Fill(Spare::kHere, p != ~0u && span.Contains(p) ? p : ~0u);
    }

    void Voice::Stop()
    {
        running_ = waiting_ = ended_ = false;
        from_ = ~0u;
        gate_ = false;
        env_.Init(sr_); // silent at once
        env_.SetSustainLevel(1.f);
        env_.SetAttackTime(attack_, 0.f);
        env_.SetReleaseTime(release_);
    }

    // the first frames once there are enough
    bool Voice::Prime()
    {
        if(!use_ram_)
        {
            Snap();
            if(!ring_ok_ || (avail_ < 3 && done_ == Ring::kOpen))
                return false;
        }
        float l, r;
        if(!Next(l, r))
        {
            if(ended_)
                Stop();
            return false;
        }
        hist_[0][0] = hist_[1][0] = l;
        hist_[0][1] = hist_[1][1] = r;
        for(int k = 2; k < 4; k++)
            if(!Next(hist_[k][0], hist_[k][1]))
            {
                hist_[k][0] = hist_[k - 1][0];
                hist_[k][1] = hist_[k - 1][1];
            }
        phase_ = 0.f;
        jump_ = kNoJump;
        return true;
    }

    // the next frame in play order; false when there is none yet, or none more (ended_)
    bool Voice::Next(float &l, float &r)
    {
        if(ended_)
            return false;
        if(use_ram_)
        {
            const Span &span = span_;
            if(span.Empty())
            {
                ended_ = true;
                return false;
            }
            const bool inside = span.Contains(pos_);
            if(!inside) // the points moved past it: on where it is while it fades, then to the start or the end
            {
                if(pos_ >= ram_->length / 2)
                {
                    ended_ = true;
                    return false;
                }
                Request(span.loop ? Action::kJump : Action::kStop);
            }
            if(gap_)
                jump_ = 3;
            const int16_t *f = ram_->mem + ram_->start + pos_ * 2;
            l = s162f(f[0]);
            r = s162f(f[1]);
            uint32_t p = pos_;
            if(!inside)
                p = span.reverse ? pos_ - 1 : pos_ + 1;
            else if(!span.Next(p))
                ended_ = true; // this was the last
            gap_ = p != (span.reverse ? pos_ - 1 : pos_ + 1);
            pos_ = p;
            return true;
        }

        if(back_ && idx_ + 1 == floor_) // played back to floor_: the spare goes on
        {
            if(floor_ends_ || spare_ != Spare::kHere) // or the file changed meanwhile
            {
                ended_ = true;
                return false;
            }
            Swap();
        }
        if(!ring_ok_)
            return false;
        const Ring &g = Cur();
        const uint32_t n = idx_;
        if(!back_)
        {
            if(n >= done_)
            {
                ended_ = true;
                return false;
            }
            if(n >= avail_)
                return false;
        }
        const int16_t *f = &g.frames[(n & kMask) * 2];
        l = s162f(f[0]);
        r = s162f(f[1]);
        if(back_)
        {
            if(next_jump_ != ~0u && n + 1 == next_jump_)
            {
                jump_ = 3;
                next_jump_ = g.PrevJump(n);
            }
            idx_ = n - 1;
            return true;
        }
        if(next_jump_ == ~0u || n > next_jump_)
            next_jump_ = g.NextJump(n);
        if(n == next_jump_)
            jump_ = 3;
        idx_ = n + 1;
        return true;
    }

    // moves on up to n frames without reading them, short of a jump or the end; how many
    uint32_t Voice::Skip(uint32_t n)
    {
        if(ended_)
            return 0;
        if(use_ram_)
        {
            if(gap_ || !span_.Contains(pos_))
                return 0;
            const uint32_t k = std::min(n, span_.Left(pos_));
            pos_ = span_.reverse ? pos_ - k : pos_ + k;
            return k;
        }
        if(!ring_ok_)
            return 0;
        const Ring &g = Cur();
        if(back_)
        {
            const uint32_t low = next_jump_ != ~0u && next_jump_ <= idx_ + 1 ? std::max(next_jump_, floor_) : floor_;
            const uint32_t k = std::min(n, idx_ + 1 - std::min(low, idx_ + 1));
            idx_ -= k;
            return k;
        }
        if(next_jump_ == ~0u || idx_ > next_jump_)
            next_jump_ = g.NextJump(idx_);
        const uint32_t to = std::min(std::min(avail_, done_), next_jump_);
        const uint32_t k = to > idx_ ? std::min(n, to - idx_) : 0;
        idx_ += k;
        return k;
    }

    // frames from the next one read to the first after the next jump or the end
    float Voice::Ahead() const
    {
        if(ended_ || (use_ram_ && gap_))
            return 0.f;
        if(use_ram_)
            return span_.Contains(pos_) ? span_.Left(pos_) + 1.f : 1e9f;
        if(back_) // frames down to the one before the jump, or to floor_ when the note ends there
        {
            const uint32_t to = !floor_ends_ ? next_jump_ : next_jump_ != ~0u ? std::max(next_jump_, floor_) : floor_;
            return to == ~0u || to > idx_ + 1 ? 1e9f : float(idx_ + 1 - to);
        }
        const uint32_t to = std::min(next_jump_, done_);
        return to == ~0u ? 1e9f : float(to - std::min(to, idx_));
    }

    Span Voice::RamSpan() const { return Span::Of(start_, end_, ram_->length / 2, play_reverse_, Loops()); }

    uint32_t Voice::Frames() const { return use_ram_target_ ? ram_->length / 2 : stream_->frames; }

    // the ring that doesn't play, again: from the start for the next note, or from pos the other way
    void Voice::Fill(Spare what, uint32_t pos)
    {
        const int k = cur_ ^ 1;
        Ring &g = stream_->ring[k];
        Publish();
        g.pos = pos;
        g.reverse = reverse_;
        g.prefill = what == Spare::kStart;
        g.read = 0;
        __DMB();
        seq_[k] = g.restart_seq + 1;
        g.restart_seq = seq_[k];
        spare_ = what;
        spare_pos_ = pos;
    }

    bool Voice::SpareReady() const
    {
        const int k = cur_ ^ 1;
        const Ring &g = stream_->ring[k];
        return g.ready_seq == seq_[k] && (g.written - g.read >= 3 || g.done != Ring::kOpen);
    }

    void Voice::Publish()
    {
        for(Ring &g : stream_->ring)
        {
            g.start = start_;
            g.end = end_;
            g.loop = Loops();
        }
    }

    // the spare ring was filled for other points
    void Voice::Invalidate()
    {
        Publish();
        if(spare_ == Spare::kStart)
            spare_ = Spare::kNone;
    }

    // the spare ring waits filled from the start
    void Voice::Prepare()
    {
        if(use_ram_target_ || spare_ != Spare::kNone)
            return;
        const Stream &s = *stream_;
        if(s.opened_seq != open_seq_ || !s.frames)
            return;
        if(!running_ && !waiting_)
            stream_->speed = next_speed_ * pitch_;
        Fill(Spare::kStart);
    }

    void Voice::StartPlaying(uint32_t from)
    {
        from_ = from;
        if(!Active())
        {
            speed_ = next_speed_;
            Begin(true);
            return;
        }
        trigger_ = true;
        Request(Action::kRestart);
    }

    void Voice::StopPlaying()
    {
        gate_ = false;
        trigger_ = false; // a jump under way plays on without starting the env again
    }

    void Voice::Choke()
    {
        SetDecay(.01f);
        gate_ = false;
        trigger_ = false;
        Request(Action::kStop);
    }

    void Voice::SetRamSource(daisy::RamBufferMemory *ram, bool loaded)
    {
        ram_ = ram;
        loaded_ = loaded;
    }

    void Voice::SetUsingRam(bool use, bool click)
    {
        use_ram_target_ = use;
        if(!Active())
        {
            use_ram_ = use;
            return;
        }
        if(use == use_ram_ && !click)
            return;
        trigger_ = click && (gate_ || cubbi_mode_);
        Request(Action::kRestart);
    }

    void Voice::OpenFile(const char *name, bool reset)
    {
        Stream &s = *stream_;
        strncpy(s.name, name, sizeof(s.name) - 1);
        s.name[sizeof(s.name) - 1] = '\0';
        __DMB();
        open_seq_ = s.open_seq + 1;
        s.open_seq = open_seq_;
        spare_ = Spare::kNone;
        if(reset && IsPlaying() && !use_ram_target_)
        {
            trigger_ = true;
            Request(Action::kRestart);
        }
    }

    void Voice::CloseFile() { OpenFile("", false); }

    bool Voice::IsKit() const { return loaded_ && slot_info.kit; }

    // start and end, 0..1: far enough apart for a loop
    bool Voice::Fits(float start, float end) const
    {
        const uint32_t frames = Frames(), b = start * frames, e = end * frames;
        return !frames || (b < e && e - b >= kMinLoopLen);
    }

    bool Voice::SetStartPoint(float val)
    {
        if(!Fits(val, end_))
            return false;
        SetStartPointForce(val);
        return true;
    }

    bool Voice::SetEndPoint(float val)
    {
        if(!Fits(start_, val))
            return false;
        SetEndPointForce(val);
        return true;
    }

    void Voice::SetStartPointForce(float val)
    {
        start_ = val;
        Invalidate();
    }

    void Voice::SetEndPointForce(float val)
    {
        end_ = val;
        Invalidate();
    }

    void Voice::SetAutoLoop(bool loop)
    {
        auto_loop_ = loop;
        Invalidate();
    }

    void Voice::SetReverse(bool rev)
    {
        if(rev == reverse_)
            return;
        reverse_ = rev;
        Invalidate();
        if(!Active())
            play_reverse_ = rev;
        else
            Request(Action::kFlip);
    }

    void Voice::SetVarispeed(float speed)
    {
        next_speed_ = speed;
        if(!env_.IsRunning())
            speed_ = speed;
    }

    void Voice::SetGain(float val)
    {
        gain_target_ = 2.f * val * val + .01f;
        if(!IsPlaying())
            gain_ = gain_target_;
    }

    void Voice::SetPan(float val)
    {
        pan_ = daisysp::fclamp(val, 0.f, 1.f);
        pan_r_target_ = fminf(2.f * pan_, 1.f);
        pan_l_target_ = fminf(2.f - 2.f * pan_, 1.f);
        if(!IsPlaying())
        {
            pan_l_ = pan_l_target_;
            pan_r_ = pan_r_target_;
        }
    }

    void Voice::SetAttack(float val)
    {
        attack_ = val * 20.f + .001f;
        env_.SetAttackTime(attack_, 0.f);
    }

    void Voice::SetDecay(float val)
    {
        release_ = val * 4.f + .001f;
        env_.SetReleaseTime(release_);
    }
}
