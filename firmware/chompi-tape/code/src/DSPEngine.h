/** DSPEngine
 *  Core DSP for sampling engine, looping engine, and additional DSP
 */
#pragma once
#include "daisy.h"
#include "FileStreamingManager.h"
#include "Voice.h"
#include <atomic>
#include "Streamer.h"
#include "LooperEngine.h"
#include "Onset.h"
#include "daisysp.h"
#include "DJFilter.h"
#include "Warble.h"
#include "PresetManager.h"
#include "EnvFollower.h"
#include "MicFilter.h"
#include "reverb.h"
#include "RamBuffer.h"
#include "limiter.h"
#include "InterpolatedDelayLine.h"
#include "Clock.h"
#include "SlotLoader.h"
#include "Arp.h"
#include "ArpKnob.h"
#include "Grains.h"
#include <algorithm>

using namespace daisy;
static constexpr size_t kMaxDelayTime = 48128 * 2; // stereo, > 1 seconds at 48kHz

static constexpr float kLineOutGain = .3f;
static constexpr float kHpGain = .2f;
static constexpr float kMicGain = 5.f;
static constexpr float kLineInGain = 3.f;
static constexpr float kLineMonoFloor = .001f;     // -60dBFS
static constexpr float kLineMonoSignal = .01f;     // -40dBFS
static constexpr uint32_t kLineMonoTime = 24000;   // .5s
static constexpr uint32_t kLineSettleTime = 72000; // 1.5s
static constexpr uint32_t kLineStereoTime = 2400;  // 50ms
static constexpr size_t kMaxPoly = 7;
static constexpr size_t kStreams = kMaxPoly + 1; // the voices' and the audition's

namespace daisy
{
    extern chompi::Stream voice_streams[kStreams];

    enum class VUTarget
    {
        VU_INPUT = 0,
        VU_OUTPUT,
        VU_LAST
    };

    enum class InputSource
    {
        MIC = 0,
        LINE_IN,
        RESAMPLE,
        USB, // the host's audio
        LAST,
    };

    enum class TransportMode
    {
        OFF = 0,
        LENGTH,
        STEP,
        LAST,
    };

    enum class LineMode
    {
        DETECTING = 0,
        MONO,
        STEREO,
        MONO_RIGHT,
    };

    enum class MonitorMode
    {
        HP = 0,
        BOTH,
        SEND_RET,
        LAST,
    };

    enum class VoiceMode
    {
        JAMMI,
        CUBBI,
        LAST,
    };

    struct KeyRequest
    {
        enum class Type
        {
            START,
            STOP,
            AUDITION_START, // key_: an Engine::Point
            AUDITION_STOP,
            DUMMY,
        };

        Type type_;
        float transpose_nn_;
        int key_;
        float vel_;

        /** constructor for full request data */
        KeyRequest(Type type,
                    float transpose_nn,
                    int key,
                    float vel
                  )
            : type_(type),
                transpose_nn_(transpose_nn),
                key_(key),
                vel_(vel)
        {
        }

        /** Empty, invalid request */
        KeyRequest()
            : type_(Type::DUMMY),
                transpose_nn_(0.f),
                key_(0),
                vel_(127.f)
        {
        }


    };

    // This shouldn't live here, but it's visible where it's needed, so...
    static const uint8_t kSlotNone = 100;
    static size_t const KeyToSlot(size_t buttonID)
    {
        if (buttonID == 7)
            return kSlotNone;
        else if (buttonID < 12)
            return buttonID - 6;
        else if (buttonID < 15)
            return kSlotNone;
        else if (buttonID == 15)
            return 1;
        else if (buttonID < 21)
            return buttonID - 10;
        else if (buttonID < 24)
            return kSlotNone;
        else if (buttonID < 29)
            return buttonID - 13;

        return kSlotNone;
    }


    /** @brief core engine for running entire modules audio
     *
     *  For now now abstraction for various sampling modes,
     *  possibly that can all be handled in the UI.
     *
     *  This will build out CHOMPI Mode and add the looping engine, and other fixed engines
     */
    class Engine
    {
    public:
        static constexpr size_t kMaxBlock = 48; // monitor_ and mix_ hold one

        Engine() {}
        ~Engine() {}

        FileStreamingManager* GetFileManager()
        {
            return &file_manager;
        }

        void Init(
            float samplerate, 
            daisysp::Reverb* reverb, 
            chompi::InterpolatedDelayLine::AudioSample* del,
            RamBufferMemory* loop_buff,
            RamBufferMemory* chompi_buff)
        {
            voice_mode = VoiceMode::JAMMI;
            latest_voice = 0;

            input_env_follower.Init();
            output_env_follower.Init();

            for (size_t i = 0; i < size_t(VoiceMode::LAST); i++)
                bank[i] = 0;

            reverb_ = reverb;

            reverb_->Init(samplerate);
            reverb_->SetAmount(0.f);
            reverb_->SetInputGain(.3f);
            reverb_->SetLowpass(1.f);

            del_.Init(del, kMaxDelayTime);
            del_.SetDelay(kMaxDelayTime * .5f);

            SetDelayTime(.5f);
            dly_time_ = dly_time_target_;

            mic_filter_.Init(samplerate);

            filter_.Init(samplerate);
            filter_.SetControl(.5f);
            cutoff_target_ = .5f;
            res_ = res_target_ = 0.f;

            SetSaturate(0.f);

            mgain_ = mgain_target_ = .8f;
            chompi::beat_clock.SetSampleRate(samplerate);

            warble_.Init(samplerate);
            warble_.SetFreq(.1f);
            
            dcblock_mic_in_.Init(samplerate);
            dcblock_line_in_l_.Init(samplerate);
            dcblock_line_in_r_.Init(samplerate);
            dcblock_fx_l_.Init(samplerate);
            dcblock_fx_r_.Init(samplerate);

            file_manager.Init(samplerate);

            fx_pre_loop = true;
            fx_env_ = fx_env_target_ = 1.f;
            final_lim_ = 0.f;

            resamp_env_ = resamp_env_target_ = 1.f;

            /* voices */
            for (size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].Init(&voice_streams[i], samplerate, chompi_buff);
            }
            chompi_writer.Init(chompi_buff);
            was_first_ = was_dub_ = false;

            for(size_t m = 0; m < 2; m++) // slot 15 is the recording, card or not
                for(size_t b = 0; b < 5; b++)
                    file_exists[m][b][14] = true;
            SetVoiceMode(VoiceMode::JAMMI);
            SetBank(0);
            SetVoiceSlot(15, true);

            /* looper */
            looper.Init(samplerate, loop_buff);

            point_step_ = samplerate * kPointStepMs / kMsPerS;
            audition_frames_ = samplerate * kAuditionEndMs / kMsPerS;
            audition_.Init(&voice_streams[kMaxPoly], samplerate, chompi_buff);
            audition_.RestoreDefaults();
            audition_.SetVelocity(127.f);
            audition_.SetAttack(0.f);
            audition_.SetDecay(.005f); // 20 ms release
            audition_.SetAutoLoop(false); // the point, once

            /** Final output compressors */
            lim_hp_l_.Init();
            lim_hp_r_.Init();
            lim_line_l_.Init();
            lim_line_r_.Init();
        }

        // fill chompi buffer with 2 second long cosine
        void FillDefaultSample(float samplerate)
        {
            float rads_l = 1.f;
            float rads_r = 0.f;
            float detune = 1.01f;
            float inc_r = .5f * 523.2511f * (TWOPI_F / samplerate) ; // hz -> increment in rads
            float inc_l = .5f * 523.2511f * detune * (TWOPI_F / samplerate) ;
            float envelope;

            for(size_t i = 0; i < 3 * samplerate; i++)
            {
                if (i > 24000) {
                    float decay_rate = 0.00007195578f;
                    envelope = expf(-decay_rate * (i - 24000));
                }
                else {
                    envelope = 1.f;
                }
                
                const size_t end = (4 * samplerate) - 2000;
                float gain = 1.f;
                if(i < 2000)
                    gain = i / 2000.f;
                else if(i > end)
                    gain = 1.f - (i - end) / 2000.f;

                rads_l += inc_l;
                if(rads_l >= TWOPI_F)
                    rads_l -= TWOPI_F;

                rads_r += inc_r;
                if(rads_r >= TWOPI_F)
                    rads_r -= TWOPI_F;

                float tri_l = 2.0f * fabsf(rads_l / TWOPI_F - 0.5f) - 1.0f;
                float tri_r = 2.0f * fabsf(rads_r / TWOPI_F - 0.5f) - 1.0f;
                // sum and output
                const int16_t sum_l = f2s16(gain * .4f * tri_l * envelope);
                const int16_t sum_r = f2s16(gain * .4f * tri_r * envelope);
                chompi_writer.StereoWrite(sum_l, sum_r, false, true);
            }
        }

        void ApplyFx(float* outl, float* outr, size_t size)
        {
            for(size_t i = 0; i < size; i++)
            {
                // slew controls at audio rate
                fonepole(cutoff_, cutoff_target_, .001f);
                fonepole(res_, res_target_, .001f);
                fonepole(saturate_amt_, saturate_amt_target_, .001f);

                // input gain
                fonepole(fx_env_, fx_env_target_, .001f);
                outl[i] *= fx_env_;
                outr[i] *= fx_env_;

                filter_.SetControl(cutoff_);
                filter_.SetRes(res_);

                outl[i] = dcblock_fx_l_.Process(outl[i]);
                outr[i] = dcblock_fx_r_.Process(outr[i]);

                // filter
                filter_.Process(outl[i], outr[i], &outl[i], &outr[i]);

                // then clip
                outl[i] = daisysp::SoftClip(saturate_amt_ * outl[i]); 
                outr[i] = daisysp::SoftClip(saturate_amt_ * outr[i]); 

                // reduce amplitude to prevent LUFs from blowing up
                const float gain = 1.f - daisysp::SoftClip(.4f * (saturate_amt_ - 1.f)) * .7f;
                outl[i] *= gain;
                outr[i] *= gain;

                // wow and flutter
                warble_.Process(outl[i], outr[i], &outl[i], &outr[i]);
            }

            // delay, in its own block to optimize SDRAM access
            for(size_t i = 0; i < size; i++)
            {
                // fonepole and set controls
                // dly_amt_ is a duplicate of reverb_amt_, but we want to fonepole it in this loop
                fonepole(dly_feedback_, dly_feedback_target_, .001f);
                fonepole(dly_time_, dly_time_target_, .001f);
                fonepole(dly_amt_, dly_amt_target_, .001f); 
                del_.SetDelay(dly_time_); 

                // read
                const float del_vol = dly_feedback_ < .2f ? dly_feedback_ * 5.f : 1.f;
                InterpolatedDelayLine::AudioSample del_read = del_.Read();
                const float delsig_l = s162f(del_read.l) * del_vol;
                const float delsig_r = s162f(del_read.r) * del_vol;

                // write
                const float mono_sum = (outl[i] + outr[i]) * .5f;
                const float del_in = mono_sum + delsig_r * powf(dly_feedback_, .7f);
                const InterpolatedDelayLine::AudioSample del_write = {int16_t(f2s16(del_in)), int16_t(f2s16(delsig_l))};
                del_.Write(del_write);

                // dry/wet mix
                const float wet_mix = dly_feedback_ > .25f ? .5f : 2.f * dly_feedback_; // quickly to 50%
                const float dry_mix = dly_feedback_ > .83f ? .5f : (1 - .6f * dly_feedback_); // slowly to 50%

                outl[i] = outl[i] * dry_mix + delsig_l * wet_mix;
                outr[i] = outr[i] * dry_mix + delsig_r * wet_mix;
            }

            // grains, after the delay so its repeats break up too
            chompi::grains.Process(outl, outr, size);

            // reverb, in its own block to optimize SDRAM access
            for(size_t i = 0; i < size; i++)
            {
                // fonepole and set controls
                fonepole(reverb_amt_, reverb_amt_target_, .001f);
                fonepole(reverb_time_, reverb_time_target_, .001f);
                reverb_->SetAmount(reverb_amt_ * reverb_amt_ * .8f);
                reverb_->SetTime(reverb_time_);
                reverb_->SetLowpass(reverb_amt_ * .6f + .4f);
                reverb_->SetDiffusion(reverb_amt_ * .6f);

                reverb_->Process(&outl[i], &outr[i]);
            }
        }

        // the FX, faded out while they are off; they keep running so their tails pass
        void MixFx(float **out, size_t size)
        {
            if(fabsf(fx_mix_target_ - fx_mix_) < 1e-4f) // the fade lands
                fx_mix_ = fx_mix_target_;
            if(fx_mix_ == 1.f && fx_mix_target_ == 1.f)
            {
                ApplyFx(out[0], out[1], size);
                return;
            }
            float dry[2][size];
            std::copy(out[0], out[0] + size, dry[0]);
            std::copy(out[1], out[1] + size, dry[1]);
            ApplyFx(out[0], out[1], size);
            for(size_t i = 0; i < size; i++)
            {
                fonepole(fx_mix_, fx_mix_target_, .001f);
                out[0][i] = dry[0][i] + (out[0][i] - dry[0][i]) * fx_mix_;
                out[1][i] = dry[1][i] + (out[1][i] - dry[1][i]) * fx_mix_;
            }
        }

        /** Apply the total monitor signal to the input envelope follower */
        void ApplyEnvelopeFollower(size_t size, float monitor[2][kMaxBlock])
        {
            for (size_t i = 0; i < size; i++)
                input_env_follower.Process((monitor[0][i] + monitor[1][i]) * .8f);
        }

        /** add the mic monitor into both outputs, 
            also add it to the monitor buffer, which will be used to feed the env follower at the end
        */
        void ApplyMicMonitor(const float* const* in, float **out, size_t size, float monitor[2][kMaxBlock])
        {
            for (size_t i = 0; i < size; i++)
            {
                float sig = dcblock_mic_in_.Process(in[0][i] * ingain_ * kMicGain);
                sig = mic_filter_.Process(sig);

                monitor[0][i] += sig;
                monitor[1][i] += sig;

                out[0][i] += sig;
                out[1][i] += sig;
            }
        }

        /** add the line in monitor into both outputs, 
            also add it to the monitor buffer, which will be used to feed the env follower at the end
        */
        void ApplyLineMonitor(const float* line_l, const float* line_r, float **out, size_t size, float monitor[2][kMaxBlock]) __attribute__((noinline))
        {
            const float mono_target = line_mode_ == LineMode::MONO         ? 1.f
                                      : line_mode_ == LineMode::MONO_RIGHT ? -1.f
                                                                           : 0.f;
            for (size_t i = 0; i < size; i++)
            {
                daisysp::fonepole(line_mono_, mono_target, .001f);
                const float to_right = fmaxf(line_mono_, 0.f), to_left = fmaxf(-line_mono_, 0.f);
                const float sigl = (line_l[i] + (line_r[i] - line_l[i]) * to_left) * ingain_ * kLineInGain;
                const float sigr = (line_r[i] + (line_l[i] - line_r[i]) * to_right) * ingain_ * kLineInGain;

                monitor[0][i] += sigl;
                monitor[1][i] += sigr;

                out[0][i] += sigl;
                out[1][i] += sigr;
            }
        }

        // like the line in
        void ApplyUsbMonitor(float **out, size_t size, float monitor[2][kMaxBlock])
        {
            if(!usb_in_)
                return;
            for (size_t i = 0; i < size; i++)
            {
                const float sigl = usb_in_[0][i] * ingain_;
                const float sigr = usb_in_[1][i] * ingain_;

                monitor[0][i] += sigl;
                monitor[1][i] += sigr;

                out[0][i] += sigl;
                out[1][i] += sigr;
            }
        }

        void ReadLineIn(const float* const* in, float* line_l, float* line_r, size_t size) __attribute__((noinline))
        {
            float peak_l = 0.f, peak_r = 0.f;
            for (size_t i = 0; i < size; i++)
            {
                line_l[i] = dcblock_line_in_l_.Process(in[2][i]);
                line_r[i] = dcblock_line_in_r_.Process(in[3][i]);
                peak_l = fmaxf(peak_l, fabsf(line_l[i]));
                peak_r = fmaxf(peak_r, fabsf(line_r[i]));
            }

            if(line_mode_manual_ || line_mode_ == LineMode::STEREO)
                return;

            if(line_settle_ < kLineSettleTime)
                line_settle_ += size;
            else if(peak_r > kLineMonoFloor)
            {
                if((line_right_ += size) >= kLineStereoTime)
                    line_mode_ = LineMode::STEREO;
            }
            else if(line_mode_ == LineMode::DETECTING && peak_l > kLineMonoSignal
                    && (line_left_only_ += size) >= kLineMonoTime)
                line_mode_ = LineMode::MONO;
        }

        void ResetLineMono()
        {
            line_mono_ = 0.f;
            line_left_only_ = line_right_ = line_settle_ = 0;
            line_mode_manual_ = false;
            line_mode_ = LineMode::DETECTING;
        }

        inline LineMode GetLineMode() { return line_mode_; }

        void CycleLineMode()
        {
            line_mode_manual_ = true; // first, before the audio thread detects
            line_mode_ = line_mode_ == LineMode::MONO         ? LineMode::MONO_RIGHT
                       : line_mode_ == LineMode::MONO_RIGHT   ? LineMode::STEREO
                                                              : LineMode::MONO;
        }

        bool looper_reset = false;
        bool CheckReset()
        {
            if(looper_reset)
            {
                looper_reset = false;
                return true;
            }

            return false;
        }

        // host audio for the next Process()
        void SetUsbInput(const float *const *usb) { usb_in_ = usb; }

        // the input as recorded, from the last Process()
        const float *const *RecordInput() const { return monitor_ptr_; }

        // the line out before the main volume, from the last Process()
        const float *const *MainMix() const { return mix_ptr_; }

        float old_fx_outl, old_fx_outr;
        void Process(const float *const *in, float **out, size_t size)
        {            
            // we'll use out[0] and out[1] as our working space, 
            // then copy to out[2] and out[3] at the end (and apply gain settings)
            std::fill(out[0], out[0] + size, 0.f);
            std::fill(out[1], out[1] + size, 0.f);

            StepArp(size);

            for (size_t voice = 0; voice < kMaxPoly; voice++)
                chompi_voice[voice].Prepare();
            audition_.Prepare();


            // for the ears only: the bus that joins after the FX, the looper and the recording
            float (&dry)[2][kMaxBlock] = dry_;
            std::fill(dry[0], dry[0] + size, 0.f);
            std::fill(dry[1], dry[1] + size, 0.f);

            /** voice read */
            for (size_t voice = 0; voice < kMaxPoly; voice++)
            {
                if (chompi_voice[voice].Active())
                    chompi_voice[voice].Render(out[0], out[1], size);
            }
            if(audition_.Active())
                audition_.Render(dry[0], dry[1], size);

            for(size_t i = 0; i < size; i++)
            {
                out[0][i] = daisysp::SoftClip(out[0][i]);
                out[1][i] = daisysp::SoftClip(out[1][i]);
                dry[0][i] = daisysp::SoftClip(dry[0][i]);
                dry[1][i] = daisysp::SoftClip(dry[1][i]);
            }

            // add the monitor to the working space pre-FX
            float (&monitor)[2][kMaxBlock] = monitor_;
            std::fill(monitor[0], monitor[0] + size, 0.f);
            std::fill(monitor[1], monitor[1] + size, 0.f);
            float line[2][size];
            ReadLineIn(in, line[0], line[1], size);

            if(monitor_mode == MonitorMode::BOTH)
            {
                if(in_source == InputSource::MIC)
                    ApplyMicMonitor(in, out, size, monitor);
                else if(in_source == InputSource::LINE_IN)
                    ApplyLineMonitor(line[0], line[1], out, size, monitor);
                else if(in_source == InputSource::USB)
                    ApplyUsbMonitor(out, size, monitor);
            }
            else if (monitor_mode == MonitorMode::SEND_RET && input_monitor && in_source == InputSource::MIC)
            {
                ApplyMicMonitor(in, out, size, monitor);
            }


            /** looper read + write */
            if(bake_ == BakeState::REQUESTED)
                bake_ = !fx_pre_loop && looper.StartPrint() ? BakeState::PRINTING : BakeState::REFUSED;
            looper.CheckRecordReady();
            if(looper.CheckReset())
                looper_reset = true;

            // FX off until the UI reset them after a bake
            if(bake_ == BakeState::FX_RESET)
            {
                SnapFx();
                bake_ = BakeState::OFF;
            }
            const bool fx_on = bake_ != BakeState::RESET_FX;

            //fx_xfade_pre
            if(fx_pre_loop && fx_on)
                MixFx(out, size);

            FollowDelay(size);

            looper.Process(out[0], out[1], size);

            if(!fx_pre_loop && fx_on)
                MixFx(out, size);

            if(looper.CapturePrint(out[0], out[1], size))
                bake_ = BakeState::RESET_FX;
            else if(bake_ == BakeState::PRINTING && !looper.IsPrinting())
                bake_ = BakeState::OFF;

            if(fx_env_ < .01f)
            {
                fx_pre_loop = !fx_pre_loop;
                fx_env_target_ = 1.f;
            }

            //working space (HP) copy to line out
            std::copy(out[0], out[0] + size, out[2]);
            std::copy(out[1], out[1] + size, out[3]);

            // add the dry monitor to the HPs only
            if (monitor_mode == MonitorMode::HP && input_monitor)
            {
                if (in_source == InputSource::MIC)
                    ApplyMicMonitor(in, out, size, monitor);
                if (in_source == InputSource::LINE_IN)
                    ApplyLineMonitor(line[0], line[1], out, size, monitor);
                if (in_source == InputSource::USB)
                    ApplyUsbMonitor(out, size, monitor);
            }
            else if (monitor_mode == MonitorMode::SEND_RET)
            {
                if (in_source == InputSource::USB)
                    ApplyUsbMonitor(out, size, monitor);
                else
                    ApplyLineMonitor(line[0], line[1], out, size, monitor);
            }

            // apply resample gain as appropriate (and envelope in/out of that situation)
            if(in_source == InputSource::RESAMPLE)
            {
                for(size_t i = 0; i < size; i++)
                {
                    daisysp::fonepole(resamp_env_, resamp_env_target_, .001f);

                    out[0][i] = monitor[0][i] = resamp_env_ * out[0][i];
                    out[1][i] = monitor[1][i] = resamp_env_ * out[1][i];
        
                    input_env_follower.Process((monitor[0][i] + monitor[1][i]) * .2f);

                    if(monitor_mode == MonitorMode::BOTH)
                    {
                        out[2][i] = out[0][i];
                        out[3][i] = out[1][i];
                    }
                }
            }
            else
            {
                ApplyEnvelopeFollower(size, monitor);
            }

            for(size_t i = 0; i < size; i++)
            {
                out[0][i] += dry[0][i];
                out[1][i] += dry[1][i];
                out[2][i] += dry[0][i];
                out[3][i] += dry[1][i];
            }


            /** voice write: idle, the input goes round the ring, so a recording keeps kRecRoll before its start */
            RecState rec = rec_state_;
            if(rec != RecState::kDone) // done, it waits for the UI; requests keep
            {
                const RecRequest req = rec_request_.exchange(RecRequest::kNone);
                if(req == RecRequest::kStop && rec == RecState::kArmed)
                    rec = RecState::kIdle;
                else if(req == RecRequest::kStop && rec == RecState::kRecording)
                {
                    rec = RecState::kEnding;
                    rec_tail_ = kRecRoll;
                }
                else if(req == RecRequest::kArm && rec == RecState::kIdle)
                {
                    rec = RecState::kArmed;
                    rec_settle_ = 0;
                }
                const bool crossed = rec == RecState::kArmed && (rec_settle_ += size) >= kRecSettle
                                     && GetVUSample(VUTarget::VU_INPUT) >= rec_threshold_;
                if(crossed || (req == RecRequest::kStart && (rec == RecState::kIdle || rec == RecState::kArmed)))
                {
                    StopLooperRecording();
                    // the start point: the onset before a crossing, else the press; kRecRoll before it comes along
                    const size_t have = chompi_writer.RollSize();
                    const size_t lead = std::min(have, crossed ? 2 * kRecOnset : chompi_writer.RollWritten() - rec_mark_);
                    rec_pre_ = std::min(have, lead + kRecRoll);
                    rec_start_ = (rec_pre_ - lead) / 2;
                    chompi_writer.FillFrom(kRecRing); // the ring goes in front once it's done
                    rec = RecState::kRecording;
                }
            }
            const bool record = rec == RecState::kRecording || rec == RecState::kEnding;
            for (size_t i = 0; i < size; i++)
            {
                const int16_t inl = f2s16(monitor[0][i]), inr = f2s16(monitor[1][i]);
                if(record)
                    chompi_writer.StereoWrite(inl, inr, false, true);
                else if(rec != RecState::kDone) // the ring waits as it was until the UI took the recording
                    chompi_writer.RollWrite(inl, inr);
            }
            if(record)
            {
                chompi::slot_loader.Reserve(chompi_writer.GetWriteHead());
                if(rec == RecState::kEnding)
                    rec_tail_ -= std::min<int32_t>(rec_tail_, 2 * size);
                if((rec == RecState::kEnding && !rec_tail_) || chompi_writer.WriteFullLength())
                {
                    rec_post_ = rec == RecState::kEnding ? kRecRoll - rec_tail_ : 0;
                    rec = RecState::kDone;
                }
            }
            rec_state_ = rec;

            // for USB, before the main gain
            std::copy(out[2], out[2] + size, mix_[0]);
            std::copy(out[3], out[3] + size, mix_[1]);

            /** main gain control */
            for(size_t i = 0; i < size; i++)
            {
                fonepole(mgain_, mgain_target_, .001f);
                fonepole(ingain_, ingain_target_, .001f);
                fonepole(final_lim_, final_lim_target_, .001f);

                // apply headphone and main gain
                out[0][i] *= kHpGain * mgain_;
                out[1][i] *= kHpGain * mgain_;

                // apply lineout and main gain
                out[2][i] *= kLineOutGain * mgain_;
                out[3][i] *= kLineOutGain * mgain_;

                // compress
                const float thresh = 1.f / (10.f * final_lim_ + 4.f);
                const float ratio = 1.f + final_lim_ * final_lim_ * 7.f;
                const float makeup = .9f + final_lim_ * .6f;
                const float pregain = 7.f * final_lim_ + 1.f;
                out[0][i] = lim_hp_l_.ProcessComp(out[0][i], pregain, thresh, ratio, makeup);
                out[1][i] = lim_hp_r_.ProcessComp(out[1][i], pregain, thresh, ratio, makeup);
                out[2][i] = lim_line_l_.ProcessComp(out[2][i], pregain, thresh, ratio, makeup);
                out[3][i] = lim_line_r_.ProcessComp(out[3][i], pregain, thresh, ratio, makeup);

                output_env_follower.Process((out[0][i] + out[1][i]));
            }

            // after the looper, so the beep isn't recorded. Only the first loop gets it, its grid stays on
            // for what snaps to the beat later, and it comes back with an empty looper
            chompi::beat_clock.SetBeep(looper.IsFirstRecording());
            chompi::beat_clock.Process(out, size, .5f * kHpGain * mgain_, .5f * kLineOutGain * mgain_);
        }

        void SetMonitorMode(MonitorMode mode) { monitor_mode = mode; }
        void SetTapeSlew(bool on) { looper.SetTapeSlew(on); }

        // The switch down samples: the input is monitored, the FX and the arp are off.
        void SetSwitch(bool sampling)
        {
            SetInputMonitor(sampling);
            UpdateFx();
        }

        // loop edit: START and END cut the loop, the keys are off; selecting or loading anything ends it
        bool EditLoops() const { return loop_edit_ || EditVoice().Loops(); }
        void SetLoopEdit(bool on)
        {
            loop_edit_ = on;
            snap_.Drop();
            UpdateFx();
        }
        bool LoopEdit() const { return loop_edit_; }

        // START and END edit the loop in loop edit, else the sample when it is in RAM (the recording or a loaded
        // slot).
        bool EditInRam() const
        {
            const daisy::RamBufferMemory *m = EditVoice().RamSource();
            return loop_edit_ ? !looper.IsFirstRecording() : EditVoice().UsesRam() && m && m->length;
        }
        float SamplePoint(int k) const { return k ? EditVoice().EndPoint() : EditVoice().StartPoint(); }
        float EditPoint(int k) const { return loop_edit_ ? looper.Point(k) : SamplePoint(k); }
        float EditPlayhead() const { return loop_edit_ && looper.IsPlaying() ? looper.Playhead() : -1.f; } // -1 while nothing plays
        bool EditOnTransient() const { return EditInRam() && snap_.On(EditSample(EditPoint(0))); }

        // Returns the memory holding what is edited: the loop's recording or the sample's.
        struct Region
        {
            const int16_t *mem;
            size_t begin, end;
        };
        Region EditRegion() const
        {
            if(loop_edit_)
                return {looper.Memory()->mem, looper.RecStart(), looper.RecStart() + looper.RecLen()};
            const daisy::RamBufferMemory *m = EditVoice().RamSource();
            return {m->mem, m->start, m->start + m->length};
        }

        // A point being set plays for a moment on a voice of its own: START from itself, END up to itself, and
        // over the seam when what is edited loops.
        enum class Point : uint8_t { kNone, kStart, kEnd };
        static constexpr uint32_t kAuditionStartMs = 400, kAuditionEndMs = 800, kAuditionSeamMs = 1600;
        uint32_t AuditionMs(Point p) const
        {
            if(p == Point::kStart)
                return kAuditionStartMs;
            return EditLoops() ? kAuditionSeamMs : kAuditionEndMs;
        }
        void AuditionPoint(Point p, bool on)
        {
            request_fifo.PushBack(KeyRequest(on ? KeyRequest::Type::AUDITION_START : KeyRequest::Type::AUDITION_STOP,
                                             0.f, int(p), 0.f));
        }

        // Moves START (k 0) or END by turns, a detent 10 ms: the loop's cut in loop edit, else the edited sample's
        // point. START settles on a transient. False when it didn't move.
        bool MovePoint(int k, int turns) { return loop_edit_ ? MoveCut(k, turns) : MoveSamplePoint(k, turns); }

        inline void SetInputMonitor(bool monitor)
        {
            input_monitor = monitor;
            UpdateResampleGain();
        }

        inline float GetVUSample(VUTarget target)
        { 
            if(target == VUTarget::VU_INPUT)
                return input_env_follower.GetLastSamp();
            else if(target == VUTarget::VU_OUTPUT)
                return output_env_follower.GetLastSamp();

            return 0.f;
        }

        // a waiting recording leaves the looper alone until the threshold
        void StopLooperRecording() __attribute__((noinline))
        {
            /** if the looper is overdubbing, stop it */
            if(looper.IsRecording())
                looper.ToggleRecord();

            /** if it was the first recording, stop it overdubbing :\ */
            if(looper.IsRecording())
                looper.ToggleRecord();
        }

        // audio thread, as the key scan sees the press go down, lag frames late
        void MarkRecordPress(size_t lag)
        {
            const size_t written = chompi_writer.RollWritten();
            rec_mark_ = written - std::min(written, 2 * lag);
        }

        // a threshold above 0 arms: the recording starts when the input crosses it
        void StartNewRecording(float threshold)
        {
            rec_threshold_ = threshold;
            rec_request_ = threshold > 0.f ? RecRequest::kArm : RecRequest::kStart;
        }

        inline bool IsRecordWaiting() const { return rec_state_ == RecState::kArmed; }

        // armed: the old sample stays. Recording: kRecRoll more, then TakeRecordingDone()
        void StopRecording() { rec_request_ = RecRequest::kStop; }

        // once per recording: the voices play it
        bool TakeRecordingDone()
        {
            if(rec_state_ != RecState::kDone)
                return false;
            chompi_writer.RollIn(rec_pre_);
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].RestoreDefaults();
                chompi_voice[i].StopPlaying();
                chompi_voice[i].CloseFile();
            }
            SetVoiceMode(VoiceMode::JAMMI);
            SetVoiceSlot(15, false);
            rec_state_ = RecState::kIdle;
            return true;
        }

        // the last recording's frames before its start point and after its end point
        size_t RecordStart() const { return rec_start_; }
        size_t RecordPost() const { return rec_post_ / 2; }
        size_t RecordFrames() { return chompi_writer.GetSize() / 2; }
        inline bool Recording() const { return rec_state_ == RecState::kRecording || rec_state_ == RecState::kEnding; }
        inline bool RecordEnding() const { return rec_state_ == RecState::kEnding; }

        /**
         * @brief Resets the assignment of keyboard keys to voices used in voice stealing algorithm.
         *        Call this when we change voice modes, banks, or slots 
         *        (except for some cases involving the chompi buffer)
         */
        inline void ResetVoiceKeys()
        {
            StopAllVoices();

            for(size_t i = 0; i < kMaxPoly; i++)
                playing_key[i] = 0;
        }

        /** @brief Voice stealing algorithm.
         *  Starts playback of sample in slot at desired playback ratio
         *  @param transpose_nn MIDI note number transposition to play w/ middle C being 0
         */
        void StartPlayback(float transpose_nn, int key, float vel) __attribute__((optimize("Os")))
        {
            // a kit plays a slice per key, from the lowest key up
            if(AiffSelected() && !chompi::slot_loader.Frames()) // still loading
                return;
            const bool kit = KitSelected();
            const int slice = kit ? chompi::slot_loader.Slice(int(transpose_nn) + 12) : -1;
            if(kit && slice < 0)
                return;

            /** Is this key already playing? In JAMMI a tail still sounding keeps its voice */
            int free_idx = -1;
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(playing_key[i] == key && (voice_mode == VoiceMode::CUBBI || !chompi_voice[i].IsPlaying()))
                {
                    free_idx = i;
                    break;
                }
            }

            /** Otherwise, take over an available voice with no key assigned */
            if (free_idx < 0)
            {
                for (size_t i = 0; i < kMaxPoly; i++)
                {
                    if (!chompi_voice[i].IsPlaying() && playing_key[i] == 0)
                    {
                        free_idx = i;
                        break;
                    }
                }
            }

            /** Otherwise, take the first available voice we can get */
            if (free_idx < 0)
            {
                for (size_t i = 0; i < kMaxPoly; i++)
                {
                    if (!chompi_voice[i].IsPlaying())
                    {
                        free_idx = i;
                        break;
                    }
                }
            }

            /** No available voice: a released one before a held one, the quietest of the released, else the oldest */
            if (free_idx < 0)
            {
                free_idx = 0;
                for(size_t i = 1; i < kMaxPoly; i++)
                {
                    const auto &v = chompi_voice[i], &best = chompi_voice[free_idx];
                    if(v.IsHeld() != best.IsHeld() ? !v.IsHeld()
                       : !v.IsHeld() && v.Level() != best.Level() ? v.Level() < best.Level()
                                                                   : play_start_time[i] <= play_start_time[free_idx])
                        free_idx = i;
                }
            }

            // we're only using one file for now. That gets opened on init, or on record end
            uint32_t now = System::GetNow();
            play_start_time[free_idx] = now;

            const bool copy_occurred = chompi_voice[free_idx].GetCopyOccurred();
            if(voice_mode == VoiceMode::CUBBI)
            {
                char name_buffer[32];
                size_t slot = KeyToSlot(key);
                if(slot == kSlotNone)
                    return;

                if(!file_exists[1][bank[int(voice_mode)]][slot - 1])
                    return; // no file in that slot

                // reopen the file if it's not a retrigger, or we just switched modes or banks on this voice
                if(slot != 15 && 
                    (playing_key[free_idx] != key 
                        || !chompi_voice[free_idx].GetCubbiMode()
                        || chompi_voice[free_idx].GetBank() != bank[int(voice_mode)]
                        || copy_occurred
                    ))
                {
                    GetFileNameForSlot(slot, bank[int(voice_mode)], voice_mode, name_buffer);
                    chompi_voice[free_idx].OpenFile(name_buffer, true); 
                    chompi_voice[free_idx].SetSlot(slot); 
                    chompi_voice[free_idx].SetUsingRam(false, true); 
                }
                else if(slot == 15)
                    chompi_voice[free_idx].SetUsingRam(true, true); 

                cubbi_slot_ = slot;
                latest_voice = free_idx;
                SetGlobalPitch(fabsf(cubbi_pitch));
                SetReverse(cubbi_pitch < 0.f);
                SetStartPoint(cubbi_start);
                SetEndPoint(cubbi_end);
                SetAttack(cubbi_attack);
                SetDecay(cubbi_decay);
                SetAutoLoop(cubbi_autoloop);
                SetSustainActive(cubbi_sustain);
                SetGain(cubbi_gain);
                SetPan(cubbi_pan);
            }

            playing_key[free_idx] = key;

            // hacks to fix issue with cubbi voices not changing samples sometimes
            chompi_voice[free_idx].SetCubbiMode(voice_mode == VoiceMode::CUBBI);
            chompi_voice[free_idx].SetBank(bank[int(voice_mode)]);

            chompi_voice[free_idx].SetVelocity(vel);

            if(kit)
            {
                const float frames = chompi::slot_loader.Frames();
                chompi_voice[free_idx].SetVarispeed(1.f);
                chompi_voice[free_idx].SetStartPointForce(chompi::slot_info.start[slice] / frames);
                chompi_voice[free_idx].SetEndPointForce(std::min(float(chompi::slot_info.end[slice]), frames) / frames);
            }
            else if(voice_mode == VoiceMode::JAMMI)
            {
                chompi_voice[free_idx].SetVarispeed(MidiNoteToPlaybackRatio(transpose_nn));
                if(SlotLoaded() != chompi_voice[free_idx].IsLoaded()) // a .wav loaded or dropped since the voice was set up
                    SetVoiceSource(chompi_voice[free_idx], false);
            }
            else if(voice_mode == VoiceMode::CUBBI)
                chompi_voice[free_idx].SetVarispeed(1.f);

            chompi_voice[free_idx].StartPlaying();
        }

        void StopPlayback(int key)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(playing_key[i] == key && !chompi_voice[i].IsKit()) // a kit slice plays to its end
                {
                    chompi_voice[i].StopPlaying();
                    // chompi_voice[i].JumpToStart();
                }
            }
        }

        void ProcessKeyReqs()
        {
            if (!request_fifo.IsEmpty())
            {
                KeyRequest req = request_fifo.PopFront();
                const bool allowed = ArpAllowed();

                if (req.type_ == KeyRequest::Type::START)
                {
                    if(allowed && (chompi::arp.On() || chompi::arp.Latched()))
                        chompi::arp.Hold(req.key_, req.transpose_nn_, req.vel_);
                    if(!allowed || !chompi::arp.On())
                        StartPlayback(req.transpose_nn_, req.key_, req.vel_);
                }
                else if (req.type_ == KeyRequest::Type::STOP)
                {
                    const bool kept = chompi::arp.Release(req.key_) && chompi::arp.Latched();
                    if(!kept && chompi::arp.Playing() != req.key_)
                        StopPlayback(req.key_);
                }
                else if (req.type_ == KeyRequest::Type::AUDITION_START || req.type_ == KeyRequest::Type::AUDITION_STOP)
                    Audition(Point(req.key_), req.type_ == KeyRequest::Type::AUDITION_START);
            }
        }

        /** Do SD Card stuff
            return true if there are more requests to complete
        */
        bool ProcessFileRequests()
        {
            file_manager.ProcessRequests();
            return !file_manager.request_fifo.IsEmpty();
        }

        void Prepare()
        {
            ProcessKeyReqs();
        }

        // the audition's voice plays what the points are set on, dry, while that isn't playing itself
        void Audition(Point p, bool on)
        {
            Voice &a = audition_;
            if(!on)
            {
                a.StopPlaying();
                return;
            }
            float start = 0.f, end = 1.f, pitch = 1.f;
            bool reverse = false;
            uint32_t frames;
            if(loop_edit_)
            {
                if(looper.IsFirstRecording() || looper.IsPlaying())
                    return;
                a.SetRamSource(looper.Memory(), false);
                a.SetUsingRam(true, false);
                frames = a.Frames();
            }
            else
            {
                if((voice_mode == VoiceMode::CUBBI && !cubbi_slot_) || AnyVoicesPlaying()) // no slot played yet, or it sounds
                    return;
                const Voice &v = EditVoice();
                SetAuditionSource(v);
                start = v.StartPoint();
                end = v.EndPoint();
                pitch = v.GetGlobalPitch();
                reverse = v.GetReverse();
                frames = v.Frames(); // the audition's stream may still be opening
            }
            a.SetStartPointForce(start);
            a.SetEndPointForce(end);
            a.SetReverse(reverse);
            a.SetGlobalPitch(pitch);
            a.SetVarispeed(1.f);
            // the end's, when the region loops: over the seam
            uint32_t from = ~0u;
            const bool seam = p == Point::kEnd && EditLoops();
            if(p == Point::kEnd)
            {
                const chompi::Span s = chompi::Span::Of(start, end, frames, reverse, false);
                const uint32_t k = std::min(s.end - s.begin, audition_frames_);
                from = s.reverse ? s.begin + k - 1 : s.end - k;
            }
            a.SetAutoLoop(seam);
            a.StartPlaying(from);
        }

        // the slot a voice plays, from the same memory or file; the file stays open across auditions
        void SetAuditionSource(const Voice &v)
        {
            if(v.GetSlot() == 15 || SlotLoaded())
            {
                SetVoiceSource(audition_, false);
                return;
            }
            const bool same = !audition_.GetCopyOccurred() && !audition_.UsesRam() && audition_.GetSlot() == v.GetSlot()
                              && audition_.GetBank() == v.GetBank() && audition_.GetCubbiMode() == v.GetCubbiMode();
            if(!same)
            {
                char name[32];
                GetFileNameForSlot(v.GetSlot(), v.GetBank(), voice_mode, name);
                audition_.OpenFile(name, true);
                audition_.SetSlot(v.GetSlot());
                audition_.SetBank(v.GetBank());
                audition_.SetCubbiMode(v.GetCubbiMode());
            }
            audition_.SetUsingRam(false, false);
        }

        bool IsKeyPlaying(int key)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if (playing_key[i] == key && chompi_voice[i].IsPlaying())
                {
                    return true;
                }
            }

            return false;
        }

        void SetAttack(float val)
        {
            val = powf(val, 3.f);

            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetAttack(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetAttack(val);            
            }
        }

        void SetDecay(float val)
        {
            val = powf(val, 3.f) + .01f;

            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetDecay(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetDecay(val);            
            }
        }

        void SetStartPointForce(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetStartPointForce(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                {
                    chompi_voice[i].SetStartPointForce(val);
                }
            }
        }

        void SetEndPointForce(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetEndPointForce(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                {
                    chompi_voice[i].SetEndPointForce(val);
                }
            }   
        }

        bool SetStartPoint(float val)
        {
            bool ret = false;
            if(voice_mode == VoiceMode::CUBBI)
            {
                ret = chompi_voice[latest_voice].SetStartPoint(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    ret = chompi_voice[i].SetStartPoint(val);            
            }

            return ret;
        }

        // the voice START and END are set on
        const Voice &EditVoice() const { return chompi_voice[voice_mode == VoiceMode::CUBBI ? latest_voice : 0]; }

        bool SetEndPoint(float val)
        {
            bool ret = false;
            if(voice_mode == VoiceMode::CUBBI)
            {
                ret = chompi_voice[latest_voice].SetEndPoint(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    ret = chompi_voice[i].SetEndPoint(val);            
            }

            return ret;
        }

        void SetAutoLoop(bool loop)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetAutoLoop(loop);
            }
            else
            {
                for(size_t i = 0; i< kMaxPoly; i++)
                {
                    chompi_voice[i].SetAutoLoop(loop);
                }
            }        
        }

        void ToggleAutoLoop()
        {
            for(size_t i = 0; i< kMaxPoly; i++)
            {
                chompi_voice[i].ToggleAutoLoop();
            }
        }

        bool GetAutoLoop() { return chompi_voice[latest_voice].GetAutoLoop(); }

        void ResetGlobalPitchQuant()
        {
            fifth = false;
            encoder_chunk = 0.f;
        }

        // returns the value for the encoder tracking
        float SetGlobalPitchQuantized(int16_t turns, float enc_pos)
        {
            encoder_chunk += turns * .25f;
            if(encoder_chunk >= 1.f || encoder_chunk <= -1.f)
            {
                // get current semi
                encoder_chunk = round(encoder_chunk);

                float pitch = GetGlobalPitch();
                float orig_pitch = pitch;

                // snap to fifths and octaves
                // calculate the consts via 2^(x/12) e.g. 2^(-5/12) for down 5 semis
                float mul;
                if(!GetReverse())
                {
                    if(fifth)
                        mul = encoder_chunk < 0 ? .667419927085f : 1.33483985417f;
                    else
                        mul = encoder_chunk < 0 ? .749153538438f : 1.49830707688f;
                }
                else
                {
                    if(fifth)
                        mul = encoder_chunk < 0 ? 1.33483985417f : .667419927085f;
                    else
                        mul = encoder_chunk < 0 ? 1.49830707688f : .749153538438f;                            
                }

                // jump, then do nothing if we've gone over the end
                pitch *= mul;
                if(pitch > 2.f || pitch < -2.f)
                    return enc_pos;

                fifth = !fifth;
                encoder_chunk = 0.f;

                // handle direction change
                if(pitch < .0625)
                {
                    // we weren't already in the turn-around zone
                    if(orig_pitch > .0625)
                    {
                        fifth = !fifth;
                        pitch = orig_pitch;
                        ToggleReverse();
                    }
                    // we were already in the zone, and we're headed over the middle
                    else if((GetReverse() && turns > 0) || (!GetReverse() && turns < 0))
                    {
                        fifth = !fifth;
                        pitch = orig_pitch;
                        ToggleReverse();
                    }
                }

                SetGlobalPitch(pitch);

                if(pitch < .5f) // 0 - .33
                    pitch = (pitch - .01) * 0.673469f;
                else if (pitch < 1.f ) // .33 - .66
                    pitch = (pitch - .5f) * .66f + .33f;
                else // .66 - 1
                    pitch = (pitch - 1.f) * .34f + .66f;
                
                return GetReverse() ? (1.f - pitch) * .5f : pitch * .5f + .5f;
            }

            return enc_pos;
        }

        /** unquantized pitch
            * curved s.t.
            *  0.f - .33f = .01x - .5x
            *  .33f - .66f = .5x - 1x
            *  .66f - 1.f = 1x - 2x
        */
        void SetGlobalPitchFree(float val) __attribute__((optimize("Os")))
        {
            val = val < .5f ? (.5f - val) * -2.f : (val - .5f) * 2.f; // 1 - 0 - 1

            float pitch;
            float inv = val < 0.f ? -1.f : 1.f;
            if(fabsf(val) < .33f) // .01x - .5x
                pitch = val * 1.484848f + .01f * inv;
            else if (fabsf(val) < .66f ) // .5x - 1x
                pitch = (val - .33f * inv) * 1.515151 + .5f * inv;
            else // 1x - 2x
                pitch = (val - .66 * inv) * 2.941176 + 1.f * inv;

            SetGlobalPitch(fabsf(pitch));
            SetReverse(pitch < 0.f);
        }

        void SetGlobalPitch(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetGlobalPitch(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetGlobalPitch(val);            
            }
        }
        inline float GetGlobalPitch() { return chompi_voice[latest_voice].GetGlobalPitch(); }

        void SetGain(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetGain(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetGain(val);            
            }
        }

        void SetPan(float val)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetPan(val);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetPan(val);            
            }            
        }

        float GetPan() 
        {
            if(voice_mode == VoiceMode::CUBBI)
                return chompi_voice[latest_voice].GetPan();

            return chompi_voice[0].GetPan();            
        }

        void SetReverse(bool rev)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetReverse(rev);
            }
            else
            {
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetReverse(rev);            
            }
        }
        void ToggleReverse() { SetReverse(!chompi_voice[latest_voice].GetReverse()); }
        inline bool GetReverse() { return chompi_voice[latest_voice].GetReverse(); }



        void SetSustainActive(bool sus)
        {
            if(voice_mode == VoiceMode::CUBBI)
            {
                chompi_voice[latest_voice].SetSustainActive(sus);
            }
            else
            {
                for(size_t i = 0; i< kMaxPoly; i++)
                {
                    chompi_voice[i].SetSustainActive(sus);
                }
            }        
        }

        void ToggleSustainActive() 
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].ToggleSustainActive();
            }
        }
        inline bool GetSustainActive() { return chompi_voice[latest_voice].GetSustainActive(); }

        void SetInputGain(float gain)
        {
            ingain_target_ = gain;
            UpdateResampleGain();
        }
        
        inline void SetMainGain(float gain) { mgain_target_ = gain; }
        inline void SetFinalComp(float comp) { final_lim_target_ = comp; }
        inline float GetFinalComp() { return final_lim_target_; }

        inline void SetReverb(float val) { dly_amt_target_ = reverb_amt_target_ = 1.3f * logf(val + 1.f); }
        // both 0..1, the grains fade the reverb down fast, to a subtle tail by 60%; returns the faded reverb,
        // the delay feedback follows it
        float SetReverbGrains(float rev, float grains)
        {
            chompi::grains.SetAmount(grains);
            const float x = fclamp(grains / .6f, 0.f, 1.f);
            rev *= 1.f - .7f * x * (2.f - x);
            SetReverb(rev);
            return rev;
        }
        float ArpQuarter()
        {
            const float q = DelayQuarter();
            return q > 0.f ? q : chompi::beat_clock.WheelQuarter();
        }

        void StepArp(size_t size) __attribute__((noinline, optimize("Os")))
        {
            int stop, key;
            float nn, vel;
            // a recording ended: the arp's latch ends, and with the FX into the loop the freeze
            const bool first = looper.IsFirstRecording(), dub = looper.IsRecording();
            const bool ended = (was_first_ && !first) || (was_dub_ && !dub);
            was_first_ = first;
            was_dub_ = dub;
            if(ended && fx_pre_loop)
                chompi::grains.Thaw();
            chompi::arp.Step(size, ArpAllowed(), ArpQuarter(), ended, stop, key, nn, vel);
            if(stop >= 0)
                StopPlayback(stop);
            for(int k; (k = chompi::arp.Drop()) >= 0;)
                if(k != key)
                    StopPlayback(k);
            if(key >= 0)
                StartPlayback(nn, key, vel);
        }

        // samples per quarter, 0 when the tempo is unknown
        float DelayQuarter()
        {
            float q;
            if(!looper.IsFirstRecording())
            {
                const float speed = fabsf(looper.GetPitch());
                if(loop_sync.quarter > 0.f)
                    return speed >= .25f ? loop_sync.quarter / speed : 0.f; // too slow to mean a tempo
                return chompi::beat_clock.Quarter(q, true) ? q : 0.f;
            }
            return chompi::beat_clock.Quarter(q) ? q : 0.f;
        }

        // the playhead on the grid DelayQuarter's tempo comes from, in quarters, -1 without one
        float GridQuarters()
        {
            if(!looper.IsFirstRecording() && loop_sync.quarter > 0.f)
                return looper.IsPlaying() ? looper.GetQuarters() : -1.f;
            uint32_t beat;
            float phase;
            bool midi;
            // a loop without a tempo: only MIDI clock is a grid
            if(!chompi::beat_clock.Get(beat, phase, nullptr, &midi) || (!looper.IsFirstRecording() && !midi))
                return -1.f;
            return (beat + phase) * chompi::beat_clock.QuartersPerBeat();
        }

        static constexpr int kDelayNotes = 6;
        static float DelayNoteQuarters(int note)
        {
            static const float quarters[kDelayNotes] = {.25f, .5f, .75f, 1.f, 1.5f, 2.f};
            return quarters[note];
        }
        // the FX knob, 0..1, and the delay time it sets
        static float KnobDelay(float knob) { return .99f * knob * knob * knob * kMaxDelayTime + 450; }
        static float DelayKnob(float time) { return cbrtf(std::max(0.f, time - 450.f) / (.99f * kMaxDelayTime)); }
        float DelayKnob() const { return DelayKnob(dly_time_target_); }
        static float MaxDelay() { return KnobDelay(1.f); }
        // the knob sets the reverb and the grains too
        void SetKnobReverb(float knob)
        {
            reverb_time_target_ = fclamp(knob, .05f, .97f);
            chompi::grains.SetSize(knob);
        }
        // 1% slack, so 1/2 at 60 BPM fits
        static int TopNote(float q)
        {
            int note = kDelayNotes - 1;
            while(note > 0 && DelayNoteQuarters(note) * q > MaxDelay() * 1.01f)
                note--;
            return note;
        }

        // False when the turn is free time: near a note it joins the notes, past either end it leaves them.
        bool StepDelayNote(int turns)
        {
            const float q = DelayQuarter();
            if(q <= 0.f)
                return false;
            const int top = TopNote(q);
            int note = chompi::beat_clock.DelayNote();
            if(note < 0)
            {
                const float t = dly_time_target_;
                note = turns > 0 ? 0 : top;
                while(turns > 0 && note <= top && DelayNoteQuarters(note) * q <= t * 1.001f)
                    note++;
                while(turns < 0 && note >= 0 && DelayNoteQuarters(note) * q >= t * .999f)
                    note--;
                const float next = note >= 0 && note <= top ? DelayNoteQuarters(note) * q : 0.f;
                if(!next || (turns > 0 ? next > t * 1.5f : next < t / 1.5f)) // not reached yet
                    return false;
            }
            else
            {
                note += turns > 0 ? 1 : -1;
                if(note < 0 || note > top)
                {
                    chompi::beat_clock.SetDelayNote(-1);
                    return false;
                }
            }
            chompi::beat_clock.SetDelayNote(note);
            return true;
        }

        // Audio thread. Smoothed so MIDI clock jitter doesn't warble the repeats; snaps once if the knob is untouched.
        void FollowDelay(size_t size)
        {
            chompi::Clock &c = chompi::beat_clock;
            float &smooth = c.DelayQuarter();
            const float q = DelayQuarter();
            chompi::grains.SetGrid(0.f, 0.f, -1.f);
            if(q <= 0.f)
            {
                smooth = 0.f;
                return;
            }
            if(smooth <= 0.f || fabsf(q - smooth) > smooth * .1f) // a new tempo, not jitter
                smooth = q;
            else
                smooth += (q - smooth) * size / c.SampleRate();

            int note = c.DelayNote();
            if(note < 0 && !c.DelayTouched())
            {
                c.TouchDelay();
                note = 0;
                for(int i = 1; i < kDelayNotes; i++)
                    if(fabsf(logf(DelayNoteQuarters(i) * smooth / dly_time_target_))
                       < fabsf(logf(DelayNoteQuarters(note) * smooth / dly_time_target_)))
                        note = i;
            }
            if(note < 0)
                return;
            note = std::min(note, TopNote(smooth));
            if(note != c.DelayNote())
                c.SetDelayNote(note);
            dly_time_target_ = std::min(DelayNoteQuarters(note) * smooth, MaxDelay());
            SetKnobReverb(DelayKnob());
            chompi::grains.SetGrid(DelayNoteQuarters(note), smooth, GridQuarters());
        }

        inline void SetDelayFeedback(float val) { dly_feedback_target_ = val * .9f; }
        inline void SetDelayTime(float val) {
            chompi::beat_clock.SetDelayNote(-1); // first, the audio thread would rewrite the time
            dly_time_target_ = KnobDelay(val);
            SetKnobReverb(val);
        }

        inline void SetWarble(float val) { warble_.SetFreq(val); }

        inline void SetFilter(float val) { cutoff_target_ = val; }
        inline void SetFilterResonance(float val) { res_target_ = val; }
        inline void SetSaturate(float val) 
        { 
            val = logf(1.7f * val + 1.f); 
            saturate_amt_target_ = val * 13.f + 1.f; 
            // saturate_amt_target_ *= .5f; 
        
        }

        inline bool GetLooperRecordArm() { return looper.GetRecordArm(); }
        inline bool GetLooperIsEmpty() { return looper.GetIsEmpty(); }
        inline float GetLooperPosition() { return looper.GetPosition(); }
        inline float GetLooperPitch() { return looper.GetPitch(); }

        inline void LooperRecordButton(bool rising) { looper.RecordButton(rising); }
        inline void LooperPlayButton(bool rising) { looper.PlayButton(rising); }

        inline void ToggleLooperRecord() { looper.TriggerRecord(); }
        inline void ToggleLooperPlaying() { looper.TogglePlaying(); }
        inline bool GetLooperReverse() { return looper.GetReverse(); }

        void ResetLooperPitchQuant()
        {
            looper_fifth = false;
            looper_encoder_chunk = 0.f;
        }

        float SetLooperPitchQuantized(int16_t turns, float enc_pos)
        {
            if (IsLooperPlaying())
            {
                looper_encoder_chunk += turns * .25f;
                if(looper_encoder_chunk >= 1.f || looper_encoder_chunk <= -1.f)
                {
                    // get current semi
                    looper_encoder_chunk = round(looper_encoder_chunk);

                    float pitch = GetLooperPitch();
                    float orig_pitch = pitch;

                    // snap to fifths and octaves
                    // calculate the consts via 2^(x/12) e.g. 2^(-5/12) for down 5 semis
                    float mul;
                    if(pitch > 0.f)
                    {
                        if(looper_fifth)
                            mul = looper_encoder_chunk < 0 ? .667419927085f : 1.33483985417f;
                        else
                            mul = looper_encoder_chunk < 0 ? .749153538438f : 1.49830707688f;
                    }
                    else
                    {
                        if(looper_fifth)
                            mul = looper_encoder_chunk < 0 ? 1.33483985417f : .667419927085f;
                        else
                            mul = looper_encoder_chunk < 0 ? 1.49830707688f : .749153538438f;                            
                    }

                    // snap to next note, return of out of bounds
                    pitch *= mul;
                    if(pitch > 2.f || pitch < -2.f)
                        return enc_pos;

                    looper_fifth = !looper_fifth;
                    looper_encoder_chunk = 0.f;

                    // handle reverse
                    if(pitch < .0625 && pitch > -.0625)
                    {
                        // we weren't already in the turn-around zone
                        if(orig_pitch > .0625 || orig_pitch < -.0625)
                        {
                            looper_fifth = !looper_fifth;
                            pitch = orig_pitch;
                            pitch *= -1.f;
                        }
                        // we were already in the zone, and we're headed over the middle
                        else if((GetLooperReverse() && turns > 0) || (!GetLooperReverse() && turns < 0))
                        {
                            looper_fifth = !looper_fifth;
                            pitch = orig_pitch;
                            pitch *= -1.f;
                        }
                    }

                    SetLooperPitch(pitch);

                    return pitch * .25f + .5f;

                }
            }

            return enc_pos;
        }

        inline void SetLooperPitchFree(float val)
        {
            SetLooperPitch(val * 4.f - 2.f); // -2 - 2            
        }

        inline void SetLooperPitch(float val) { looper.SetPitch(val); }
        inline void SetLooperScrub(float scrub) { looper.SetScrub(scrub); }
        inline float GetLooperScrub() { return looper.GetScrub(); }

        bool AnyVoicesPlaying() 
        {
            for(size_t i = 0; i < kMaxPoly; i++)
            { 
                if(chompi_voice[i].IsPlaying())
                    return true;
            }

            return false;
        }

        inline bool IsLooperPlaying() { return looper.IsPlaying(); };
        inline bool IsLooperRecording() { return looper.IsRecording(); };
        inline bool IsLooperPrinting() { return looper.IsPrinting(); }

        inline void BakeFx()
        {
            if(bake_ == BakeState::OFF)
                bake_ = BakeState::REQUESTED;
        }

        bool TakeBakeRefused()
        {
            if(bake_ != BakeState::REFUSED)
                return false;
            bake_ = BakeState::OFF;
            return true;
        }

        inline bool IsFxResetDue() { return bake_ == BakeState::RESET_FX; }

        void FxResetDone()
        {
            __DMB(); // FX targets before the audio thread snaps to them
            bake_ = BakeState::FX_RESET;
        }

        void SnapFx()
        {
            cutoff_ = cutoff_target_;
            res_ = res_target_;
            saturate_amt_ = saturate_amt_target_;
            dly_time_ = dly_time_target_;
            dly_feedback_ = dly_feedback_target_;
            dly_amt_ = dly_amt_target_;
            reverb_amt_ = reverb_amt_target_;
            reverb_time_ = reverb_time_target_;
            warble_.SnapMix();
            chompi::grains.Snap();
        }
        inline bool IsLooperFirstRecording() { return looper.IsFirstRecording(); };
        inline bool IsLooperRecordArmed() { return looper.GetRecordArm(); };


        inline void IncrementLooperDubGain(float gain) { looper.IncrementDubGain(gain); }
        inline float GetLooperDubGain() { return looper.GetDubGain(); }

        inline void LooperOpenFile(bool play)
        {
            SourceChanged();
            looper.OpenFile(play);
        }
        inline void LooperStop(bool play = false) { looper.Stop(play); }
        inline void LooperLengthChange(int8_t steps) { looper.RequestLengthChange(steps); }
        inline void LooperResetLength() { looper.RequestLengthChange(-looper.GetLengthExp()); }
        inline int8_t GetLooperLengthExp() { return looper.GetLengthExp(); }
        inline void CycleTransportMode() { transport_mode = TransportMode((int(transport_mode) + 1) % int(TransportMode::LAST)); }
        inline TransportMode GetTransportMode() { return transport_mode; }
        inline void LooperStep(int8_t dir) { looper.RequestStep(dir); }
        inline void LooperMakeLengthBase() { looper.RequestLengthBase(); }
        inline size_t GetLooperSection() { return looper.GetSection(); }
        inline bool LooperUndo() { return looper.RequestUndo(); }

        inline int GetBank() { return bank[int(voice_mode)];}
        inline size_t GetVoiceBank() 
        {
            if(voice_mode == VoiceMode::CUBBI)
                return bank[int(voice_mode)];
            
            return voice_bank_; 
        }
        inline void IncrementBank() 
        {
            bank[int(voice_mode)] = (bank[int(voice_mode)] + 1) % 5;

            if(voice_mode == VoiceMode::CUBBI)
                ResetVoiceKeys();
        }
        inline void SetBank(size_t b)
        {
            SourceChanged();
            bank[int(voice_mode)] = b % 5;

            if(voice_mode == VoiceMode::CUBBI)
                ResetVoiceKeys();
        }

        daisy::FIFO<KeyRequest, 32> request_fifo;

        /** @brief populates expected filename for a given slot
         *  @param slot position in the bank
         *  @param bank bank number
         *  @param name string to fill with filename
         *
         *  name must be a buffer of at least 16 bytes to fit the name:
         *  - "chompi_xy.wav" where xy is a letter-number combo indicating bank/slot
         */
        static void GetFileNameForSlot(int slot, int b, VoiceMode m, char *name)
        {
            char bankchar;
            switch(b)
            {
                case 0:
                    bankchar = 'a';
                    break;
                case 1:
                    bankchar = 'b';
                    break;
                case 2:
                    bankchar = 'c';
                    break;
                case 3:
                    bankchar = 'd';
                    break;
                default:
                    bankchar = 'e';
            }

            char mode[10];

            if(m == VoiceMode::JAMMI)
                strcpy(mode, "jammi");
            else if(m == VoiceMode::CUBBI)
                strcpy(mode, "cubbi");

            if (name)
            {
                sprintf(name, "%s_%c%1d.wav", mode, bankchar, slot);
            }
        }

        char fname[32];
        char fname_json[32];
        uint8_t erasing = 0;
        void EraseStart(uint8_t target, int b, VoiceMode m)
        {
            const bool kit = IsKitSlot(target, b, m);
            SetFileExists(target - 1, b, m, false);

            GetFileNameForSlot(target, b, m, fname);
            if(kit) // no .wav there, its .aif and sidecar instead
                KitName(fname);
            chompi::slot_loader.Forget(fname);
            chompi::streamer.Forget();

            FileRequest req(FileRequest::Type::UNLINK, nullptr, fname, 0, this);
            file_manager.request_fifo.PushBack(req);

            if(kit)
            {
                chompi::KitFileName(fname_json, fname, ".json");
                FileRequest json_req(FileRequest::Type::UNLINK, nullptr, fname_json, 0, this);
                file_manager.request_fifo.PushBack(json_req);
            }
            erasing += kit ? 2 : 1;
        }

        inline void EraseFinished()
        { 
            if(erasing != 0)
                erasing--; 
        }

        inline bool IsErasing() { return erasing != 0; }

        inline void SetAllCopyOccurred()
        {
            chompi::streamer.Forget();
            for(size_t i = 0; i < kMaxPoly; i++)
                chompi_voice[i].SetCopyOccurred();
        }

        // one pass over the card's folder: which slots have a sample or a kit
        void UpdateFileExists() __attribute__((optimize("Os")))
        {
            bool wav[2][5][14] = {}, aif[5][14] = {};
            DIR dir;
            FILINFO info;
            if(f_opendir(&dir, "") == FR_OK)
            {
                size_t m, b, s;
                bool kit;
                while(f_readdir(&dir, &info) == FR_OK && info.fname[0])
                    if(SlotOfFile(info.fname, m, b, s, kit))
                        (kit ? aif[b][s] : wav[m][b][s]) = true;
                f_closedir(&dir);
            }

            for(size_t m = 0; m < 2; m++)
                for(size_t b = 0; b < 5; b++)
                    for(size_t s = 0; s < 14; s++)
                        SetSlotFiles(s, b, VoiceMode(m), wav[m][b][s], aif[b][s]);
        }

        // one slot (0..13), read from the card
        void UpdateFileExists(size_t slot, size_t bank, VoiceMode mode)
        {
            if(slot >= 14)
                return;
            char name[32];
            GetFileNameForSlot(slot + 1, bank, mode, name);
            const bool wav = f_stat(name, nullptr) == FR_OK;
            KitName(name);
            SetSlotFiles(slot, bank, mode, wav, f_stat(name, nullptr) == FR_OK);
        }

        // a slot holds its .wav, else in JAMMI an OP-1 .aif
        void SetSlotFiles(size_t slot, size_t bank, VoiceMode mode, bool wav, bool aif)
        {
            SetFileExists(slot, bank, mode, wav);
            if(!wav && aif && mode == VoiceMode::JAMMI)
            {
                file_exists[int(mode)][bank][slot] = true;
                chompi::kit_slots[bank] |= 1 << (slot + 1);
            }
        }

        // the slot a file is for, by the names GetFileNameForSlot and KitName give, ignoring case
        static bool SlotOfFile(const char *name, size_t &m, size_t &b, size_t &s, bool &aif)
        {
            char slot_name[32];
            for(m = 0; m < 2; m++)
                for(b = 0; b < 5; b++)
                {
                    GetFileNameForSlot(1, b, VoiceMode(m), slot_name);
                    const size_t prefix = strlen(slot_name) - strlen("1.wav");
                    if(strncasecmp(name, slot_name, prefix))
                        continue;
                    const int slot = atoi(name + prefix);
                    if(slot < 1 || slot > 14)
                        return false;
                    s = slot - 1;
                    GetFileNameForSlot(slot, b, VoiceMode(m), slot_name);
                    aif = false;
                    if(!strcasecmp(name, slot_name))
                        return true;
                    KitName(slot_name);
                    aif = true;
                    return m == size_t(VoiceMode::JAMMI) && !strcasecmp(name, slot_name);
                }
            return false;
        }

        inline void SetFileExists(size_t slot, size_t bank, VoiceMode mode, bool exists)
        {
            file_exists[int(mode)][bank][slot] = exists;
            if(mode == VoiceMode::JAMMI) // a new .wav there wins over an .aif
                chompi::kit_slots[bank] &= ~(1 << (slot + 1));
        }

        // the start and end knobs move the last played slice of a kit, 2 ms a step
        bool EditKit(bool end, int turns)
        {
            if(!KitSelected())
                return false;
            chompi::slot_loader.Edit(end, turns * int(chompi::slot_info.rate * .002f), System::GetNow());
            return true;
        }

        bool ArpAllowed() { return voice_mode == VoiceMode::JAMMI && !KitSelected() && !input_monitor; }

        // the JAMMI slot plays an .aif
        bool AiffSelected() { return voice_mode == VoiceMode::JAMMI && aiff_slot_; }
        bool KitSelected() { return AiffSelected() && chompi::slot_info.kit; }

        // a JAMMI slot that plays an OP-1 .aif
        static bool IsKitSlot(size_t slot, size_t bank, VoiceMode mode)
        {
            return mode == VoiceMode::JAMMI && slot < 15 && chompi::kit_slots[bank] >> slot & 1;
        }
        static void KitName(char *name) { chompi::KitFileName(name, name, ".aif"); }

        inline bool GetFileExists(size_t slot)
        { 
            return file_exists[int(voice_mode)][GetBank()] [slot];
        }
        bool GetFileExists(size_t slot, size_t bank, VoiceMode mode) const { return file_exists[int(mode)][bank][slot]; }

        inline size_t GetVoiceSlot()
        { 
            if(voice_mode == VoiceMode::JAMMI)
                return voice_slot_;
            
            return cubbi_slot_;
        }

        // these should really be in a struct defined in the presets file
        float cubbi_pitch;
        float cubbi_start;
        float cubbi_end;
        float cubbi_attack;
        float cubbi_decay;
        bool cubbi_autoloop;
        bool cubbi_sustain;
        float cubbi_gain;
        float cubbi_pan;

        void OpenCubbiSlot(float pitch, float start, float end, float attack, float decay, bool autoloop, bool sustain, float gain, float pan)
        {
            cubbi_pitch = pitch;
            cubbi_start = start;
            cubbi_end = end;
            cubbi_attack = attack;
            cubbi_decay = decay;
            cubbi_autoloop = autoloop;
            cubbi_sustain = sustain;
            cubbi_gain = gain;
            cubbi_pan = pan;
        }

        void SetVoiceSlot(size_t idx, bool click)
        {
            SourceChanged();
            voice_slot_ = idx;
            
            if(voice_mode == VoiceMode::JAMMI)
            {
                voice_bank_ = bank[int(voice_mode)];
            }

            char name_buffer[32];
            GetFileNameForSlot(idx, bank[int(voice_mode)], voice_mode, name_buffer);
            const bool kit = IsKitSlot(idx, bank[int(voice_mode)], voice_mode); // an .aif, only from RAM
            aiff_slot_ = kit;
            if(kit)
                KitName(name_buffer);
            if(voice_mode == VoiceMode::JAMMI && idx != 15) // a .wav streams until it is in RAM
                chompi::slot_loader.Request(name_buffer);

            for(size_t i = 0; i < kMaxPoly; i++)
                if(chompi_voice[i].IsLoaded() || kit) // slot_buff gets the new slot, no key up ends a kit slice
                    chompi_voice[i].StopPlaying();
            if(idx != 15 && !kit)
                OpenFiles(name_buffer);
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                chompi_voice[i].SetSlot(idx);
                chompi_voice[i].SetCubbiMode(voice_mode == VoiceMode::CUBBI);
                SetVoiceSource(chompi_voice[i], click);
            }
        }

        // every voice streams the file
        void OpenFiles(const char *name)
        {
            for(size_t i = 0; i < kMaxPoly; i++)
                chompi_voice[i].OpenFile(name, true);
        }

        // the JAMMI slot plays from slot_buff: an .aif always, a .wav once it is loaded
        bool SlotLoaded()
        {
            return voice_mode == VoiceMode::JAMMI && voice_slot_ != 15 && (aiff_slot_ || chompi::slot_loader.Loaded());
        }

        // from RAM: the loaded slot or the recording (slot 15); else from the card. Not inlined, it saves code room.
        void SetVoiceSource(Voice &v, bool click) __attribute__((noinline))
        {
            const bool loaded = SlotLoaded();
            v.SetRamSource(loaded ? &chompi::slot_buff : chompi_writer.Memory(), loaded);
            v.SetUsingRam(loaded || voice_slot_ == 15, click);
        }


        inline VoiceMode GetVoiceMode () { return voice_mode; }

        inline void SetVoiceMode(VoiceMode m)
        {
            SourceChanged();
            if(voice_mode != m)
                ResetVoiceKeys();
            if(m != VoiceMode::JAMMI) // kits are JAMMI only
                for(size_t i = 0; i < kMaxPoly; i++)
                    chompi_voice[i].SetRamSource(chompi_writer.Memory(), false);

            voice_mode = m;
        }

        void StopAllVoices(size_t idx = 100)
        {
            if(idx == 100)
                chompi::arp.Clear();
            for(size_t i = 0; i < kMaxPoly; i++)
            {
                if(idx == 100 || idx == chompi_voice[i].GetSlot())
                    chompi_voice[i].Choke();
            }
        }

        // the card is gone: what streams from it stops, what is in RAM plays on
        void StopStreams()
        {
            for(size_t i = 0; i < kMaxPoly; i++)
                if(!chompi_voice[i].UsesRam())
                    chompi_voice[i].Choke();
            if(!audition_.UsesRam())
                audition_.Choke();
        }
        // the card is back: what streams opens its file anew; in CUBBI each voice has its own, the next key does it
        void ReopenStreams()
        {
            audition_.SetCopyOccurred();
            if(voice_mode == VoiceMode::CUBBI)
            {
                SetAllCopyOccurred();
                return;
            }
            if(voice_slot_ == 15 || aiff_slot_)
                return;
            char name_buffer[32];
            GetFileNameForSlot(voice_slot_, bank[int(voice_mode)], voice_mode, name_buffer);
            OpenFiles(name_buffer);
        }
        // otherwise, they are post looper
        void SetFxPreLooper(bool pre)
        {
            if(pre != fx_pre_loop)
            {
                looper.FXEnvelope();
                fx_env_target_ = 0.f;
            }
        }

        inline bool GetFxPreLooper() { return fx_pre_loop; }

        inline InputSource GetInputSource() { return in_source; }

        void SetInputSource(InputSource source)
        {
            in_source = source;
        }

        // a key or the jack: USB doesn't take over from it until the host connects anew
        void PickInputSource(InputSource source)
        {
            in_source = source;
            source_picked_ = true;
        }

        // the host playing makes USB the input unless a source was picked since it connected, until it goes away
        void UpdateUsbInputSource(bool connected, bool streaming, bool line_in)
        {
            if(connected != usb_connected_)
            {
                source_picked_ = false;
                if(!connected && in_source == InputSource::USB)
                    in_source = line_in ? InputSource::LINE_IN : InputSource::MIC;
            }
            usb_connected_ = connected;

            if(streaming && !usb_streaming_ && !source_picked_)
                in_source = InputSource::USB;
            usb_streaming_ = streaming;
        }

        inline void IncrementMonitorMode() { monitor_mode = MonitorMode( (int(monitor_mode) + 1) % int(MonitorMode::LAST) ); }
        inline MonitorMode GetMonitorMode() { return monitor_mode; }

        inline bool IsManagerEmpty() __attribute__((optimize("-O0"))) { return file_manager.request_fifo.IsEmpty(); }

        // helper for stuck key hack
        inline int GetPlayingKey(size_t idx) { return playing_key[idx]; }

    private:
        // The FX run with the switch up and out of loop edit.
        void UpdateFx() { fx_mix_target_ = input_monitor || loop_edit_ ? 0.f : 1.f; }

        // Resampling plays through the input gain while the input is monitored; the gain is applied in RESAMPLE
        // alone, so it is kept right whatever the source.
        void UpdateResampleGain() { resamp_env_target_ = input_monitor ? ingain_target_ : 1.f; }

        // Another source is edited: loop edit ends and the snap lets go.
        void SourceChanged()
        {
            loop_edit_ = false;
            snap_.Drop();
            UpdateFx();
        }

        // Moves the loop's START or END by turns and snaps START onto a transient. False when it didn't move.
        bool MoveCut(int k, int turns)
        {
            const size_t was = looper.Cut(k);
            if(!looper.MoveCut(k, turns, point_step_))
                return false;
            if(k == 0)
                looper.SetCut(0, Settle(was, looper.Cut(0)));
            return true;
        }

        // Moves START or END of the edited sample by turns; START settles on a transient. False when it didn't
        // move. The voices and the presets keep the points as fractions of the sample.
        bool MoveSamplePoint(int k, int turns)
        {
            const Voice &v = EditVoice();
            const uint32_t frames = v.Frames();
            if(!frames)
                return false;
            const float was = SamplePoint(k);
            int64_t at = int64_t(double(was) * frames + .5) + int64_t(turns) * point_step_;
            at = std::max<int64_t>(0, std::min<int64_t>(frames, at));
            if(k == 0 && EditInRam())
            {
                const Region r = EditRegion();
                at = (Settle(EditSample(was), r.begin + 2 * size_t(at)) - r.begin) / 2;
            }
            const float f = float(double(at) / frames);
            return f != was && (k ? SetEndPoint(f) : SetStartPoint(f));
        }

        // a fraction of what's edited, as a sample index of its memory
        size_t EditSample(float f) const
        {
            const Region r = EditRegion();
            return r.begin + 2 * size_t(double(f) * ((r.end - r.begin) / 2)); // in double, a point stays on its frame
        }

        // START moved from was to at: where it settles, on a transient or not
        size_t Settle(size_t was, size_t at)
        {
            const Region r = EditRegion();
            return snap_.Settle(r.mem, r.begin, r.end, was, at);
        }
        /** @brief Converts a MIDI note number to a ratio of playback speed
         *  @param nn number of MIDI notes to transpose above or below original speed
         *
         *  @todo handle as a value from 0-127 w/ Middle C being 0
         *  @todo handle w/ lookup table to improve performance
         */
        float MidiNoteToPlaybackRatio(int nn) { return pow(2.f, nn * 0.08333333f); } // .0833 = 1/12

        /** the looper */
        LooperEngine looper;
        TransportMode transport_mode = TransportMode::OFF;

        float encoder_chunk = 0.f; // chunk up the quantized pitch controls
        float looper_encoder_chunk = 0.f;
        bool fifth, looper_fifth;
        
        /** env followers for VU meters */
        EnvFollower input_env_follower;
        EnvFollower output_env_follower;

        /** Sampling Bits */
        daisy::FileStreamingManager file_manager;
        chompi::Voice chompi_voice[kMaxPoly];
        RamBuffer chompi_writer;

        chompi::Limiter lim_hp_l_;
        chompi::Limiter lim_hp_r_;
        chompi::Limiter lim_line_l_;
        chompi::Limiter lim_line_r_;



        bool was_first_, was_dub_; // looper state in the last block
        bool input_monitor;
        // the recording: the UI requests, the audio thread runs the state and hands a done one back
        enum class RecRequest : uint8_t { kNone, kArm, kStart, kStop };
        enum class RecState : uint8_t { kIdle, kArmed, kRecording, kEnding, kDone };
        std::atomic<RecRequest> rec_request_{RecRequest::kNone};
        std::atomic<RecState> rec_state_{RecState::kIdle};
        float rec_threshold_ = 0.f;
        size_t rec_settle_ = 0;
        int32_t rec_tail_ = 0; // samples still to record after the stop
        size_t rec_mark_ = 0;                            // ring samples at the press
        size_t rec_pre_ = 0, rec_start_ = 0, rec_post_ = 0;
        int playing_key[kMaxPoly];

        VoiceMode voice_mode;

        bool file_exists[2][5][15]; // mode, bank, slot
        size_t voice_slot_;
        bool aiff_slot_ = false; // the voices were set up for an .aif, a copy may change kit_slots meanwhile
        size_t cubbi_slot_;

        // in jammi mode, which bank is currently active?
        // we can change bank pages without selecting a new slot in that bank
        size_t voice_bank_;

        uint32_t play_start_time[kMaxPoly];

        float ingain_, ingain_target_;
        float resamp_env_, resamp_env_target_;
        float mgain_, mgain_target_;
        float final_lim_, final_lim_target_;

        /** FX */
        MicFilter mic_filter_;
        DjFilter filter_;
        daisysp::Reverb* reverb_;
        chompi::InterpolatedDelayLine del_;
        Warble warble_;
        daisysp::DcBlock dcblock_mic_in_;
        daisysp::DcBlock dcblock_line_in_l_;
        daisysp::DcBlock dcblock_line_in_r_;
        float line_mono_ = 0.f;
        uint32_t line_left_only_ = 0;
        uint32_t line_right_ = 0;
        uint32_t line_settle_ = 0;
        // volatile: the UI thread can set these by hand
        volatile LineMode line_mode_ = LineMode::DETECTING;
        volatile bool line_mode_manual_ = false;
        daisysp::DcBlock dcblock_fx_l_;
        daisysp::DcBlock dcblock_fx_r_;

        size_t latest_voice;

        float dly_time_, dly_time_target_;
        float dly_feedback_, dly_feedback_target_;
        float reverb_amt_, reverb_amt_target_;
        float reverb_time_, reverb_time_target_;
        float dly_amt_, dly_amt_target_;
        float saturate_amt_, saturate_amt_target_;
        float cutoff_, cutoff_target_;
        float res_, res_target_;
        
        bool fx_pre_loop;
        float fx_env_, fx_env_target_;
        float fx_mix_ = 1.f, fx_mix_target_ = 1.f;
        bool loop_edit_ = false;
        Voice audition_;
        chompi::Snap snap_;
        static constexpr float kMsPerS = 1000.f;
        static constexpr uint32_t kPointStepMs = 10;
        uint32_t point_step_;      // frames a detent moves a point
        uint32_t audition_frames_; // END's audition starts this far before the end
        // UI writes OFF, REFUSED and RESET_FX, the audio thread the rest
        enum class BakeState { OFF, REQUESTED, REFUSED, PRINTING, RESET_FX, FX_RESET };
        volatile BakeState bake_ = BakeState::OFF;

        int bank[int(VoiceMode::LAST)];

        InputSource in_source;
        volatile bool source_picked_ = false;
        bool usb_connected_ = false, usb_streaming_ = false;
        const float *const *usb_in_ = nullptr;
        float monitor_[2][kMaxBlock];
        float dry_[2][kMaxBlock];
        const float *monitor_ptr_[2] = {monitor_[0], monitor_[1]};
        float mix_[2][kMaxBlock];
        const float *mix_ptr_[2] = {mix_[0], mix_[1]};
        MonitorMode monitor_mode = MonitorMode::HP;

        /** No copy, no assign */
        Engine(const Engine &);
        void operator=(const Engine &);
    };

} // namespace chompi