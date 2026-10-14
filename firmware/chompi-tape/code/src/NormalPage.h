#pragma once

#include "hardware.h"
#include "DSPEngine.h"
#include "Leds.h"
#include "FileCopier.h"
#include "Clock.h"
#include "Hold.h"
#include "Audition.h"
#include "Onset.h"

namespace chompi
{
        static const uint8_t cc_map[3][6] = {
            {20, 21, 22, 23, 24, 25},
            {28, 29, 30, 31, 0, 32},
            {0, 0, 0, 33, 0, 0}};
        static const uint8_t kThresholdCC = 34;

        static const uint8_t key_map[40] = {
            0x01, /**< ENC_1_SW  page */
            0x02, /**< ENC_2_SW page */
            0x03, /**< ENC_3_SW page */
            0x00, /**< ENC_4_SW page */ /** TODO: was CC 16 out */
            0x04, /**< ENC_5_SW page */
            0x15, /**< KEY_26 chompi cc */
            0x00, /**< SW_TOG skip */
            0x31, /**< KEY_16 */
            0x32, /**< KEY_2 */
            0x34, /**< KEY_3 */
            0x35, /**< KEY_4 */
            0x37, /**< KEY_5 */
            0x33, /**< KEY_17 */
            0x36, /**< KEY_18 */
            0x38, /**< KEY_19 */
            0x30, /**< KEY_1 */
            0x39, /**< KEY_6 */
            0x3b, /**< KEY_7 */
            0x3c, /**< KEY_8 */
            0x3e, /**< KEY_9 */
            0x40, /**< KEY_10 */
            0x3a, /**< KEY_20 */
            0x3d, /**< KEY_21 */
            0x3f, /**< KEY_22 */
            0x41, /**< KEY_11 */
            0x43, /**< KEY_12 */
            0x45, /**< KEY_13 */
            0x47, /**< KEY_14 */
            0x48, /**< KEY_15 */
            0x42, /**< KEY_23 */
            0x44, /**< KEY_24 */
            0x46, /**< KEY_25 */
            0x05, /**< ENC_6_SW page */
            0x17, /**< KEY_27 play cc */
            0x18, /**< KEY_28 loop cc */
            0x00, /**< NC_1 skip */
            0x00, /**< NC_2 skip */
            0x00, /**< NC_3 skip */
            0x00, /**< NC_4 skip */
            0x00, /**< NC_5 skip */
        };

        static const uint8_t led_map[40]{
            2,    /**< ENC_1_SW  page */
            3,    /**< ENC_2_SW page */
            4,    /**< ENC_3_SW page */
            1,    /**< ENC_4_SW cc */
            0,    /**< ENC_5_SW page */
            0,    /**< KEY_26 chompi cc */
            0x00, /**< SW_TOG skip */
            0,    /**< KEY_16 */
            23,   /**< KEY_2 */
            22,   /**< KEY_3 */
            21,   /**< KEY_4 */
            20,   /**< KEY_5 */
            1,    /**< KEY_17 */
            2,    /**< KEY_18 */
            3,    /**< KEY_19 */
            24,   /**< KEY_1 */
            19,   /**< KEY_6 */
            18,   /**< KEY_7 */
            17,   /**< KEY_8 */
            16,   /**< KEY_9 */
            15,   /**< KEY_10 */
            4,    /**< KEY_20 */
            5,    /**< KEY_21 */
            6,    /**< KEY_22 */
            14,   /**< KEY_11 */
            13,   /**< KEY_12 */
            12,   /**< KEY_13 */
            11,   /**< KEY_14 */
            10,   /**< KEY_15 */
            7,    /**< KEY_23 */
            8,    /**< KEY_24 */
            9,    /**< KEY_25 */
            9,    /**< ENC_6_SW page */
            7,    /**< KEY_27 play cc */
            8,    /**< KEY_28 loop cc */
            0x00, /**< NC_1 skip */
            0x00, /**< NC_2 skip */
            0x00, /**< NC_3 skip */
            0x00, /**< NC_4 skip */
            0x00, /**< NC_5 skip */
        };

    // .00787 ~= what midi was. 1 / 127
    static const float kEncoderFineStep = .003f;
    static const float kEncoderCoarseStep = .01f;
    static const float kMagicTop = 2.f; // past 1 the magic knob goes into grains
    static const float kRecDim = .7f;


    static const float white[3] = {1.f, 1.f, 1.f};
    static const float red[3] = {1.f, 0.f, 0.f};
    static const float orange[3] = {1.f, .6f, .24f};
    static const float yellow[3] = {1.f, .95f, 0.05f};
    static const float green[3] = {0.f, 1.f, 0.f};
    static const float teal[3] = {.14f, 1.f, .92f};
    static const float med_blue[3] = {0.f, .84f, 1.f};
    static const float blue[3] = {0.f, 0.f, 1.f};
    static const float purple[3] = {.58f, .05f, 1.f};
    static const float pink[3] = {1.f, .36f, .62f};
    static const float magenta[3] = {1.f, 0.f, 1.f};
    static const float dark_orange[3] = {.77f, .38f, .06f};
    static const float yellow_green[3] = {.706f, 1.f, 0.f};

    // a speed, 0..1 with 1x in the middle: 0 at either end, 1 at 1x
    inline float speed_mid(float speed) { return speed < .5f ? speed * 2.f : (1.f - speed) * 2.f; }

    // red at 1x, through yellow and green to blue at either end
    inline void speed_color(float speed, float &r, float &g, float &b)
    {
        const float mid = speed_mid(speed);
        r = color_quad_xfade(med_blue[0], green[0], yellow[0], red[0], mid);
        g = color_quad_xfade(med_blue[1], green[1], yellow[1], red[1], mid);
        b = color_quad_xfade(med_blue[2], green[2], yellow[2], red[2], mid);
    }

    // the wheel at a speed: the LED on the side it goes, the other one fades in red over the last fifth to 1x;
    // k dims both
    inline void wheel_speed_leds(float speed, float k)
    {
        float r, g, b;
        speed_color(speed, r, g, b);
        leds.ui.Pth(speed > .5f ? 6 : 5, r * k, g * k, b * k);
        const float other = std::max(0.f, speed_mid(speed) - .8f) * 5.f * k;
        leds.ui.Pth(speed > .5f ? 5 : 6, red[0] * other, red[1] * other, red[2] * other);
    }

    static const uint8_t knob_num_pages[6] = {3, 2, 2, 3, 1, 2};

    class NormalPage : public daisy::UiPage
    {
    public:
        void Init(Hardware *hw, Engine *fx, FileCopier *copier, float** enc_arr, const float** def_arr,
                    uint8_t* page, PresetManager* pre)
        {
            hw_ = hw;
            fx_ = fx;
            copier_ = copier;
            presets_ = pre;
            enc_values = enc_arr;
            enc_defaults = def_arr;
            knob_page = page;

            transport_hold.Init();
            rec_hold.Init();
            magic_hold.Init();

            for (int knob = 0; knob < 6; knob++)
            {
                for (int page = 0; page < 3; page++)
                {
                    enc_values[page][knob] = enc_defaults[page][knob];
                }
            }

            enc_values[0][5] = presets_->Volume();

            fx_->SetGlobalPitch(1.f);
            fx_->SetReverse(false);
            fx_->SetSwitch(!switch_state);

            fx_->SetInputGain(.75f);


            leds.ui.Clear();

            // 8mm leds
            leds.ui.Pth(0, 0.f, 0.f, 0.f);
            leds.ui.Pth(1, green[0], green[1], green[2]);
            leds.ui.Pth(2, 0.f, 0.f, 0.f);
            leds.ui.Pth(3, 0.f, 0.f, 0.f);
            leds.ui.Pth(4, 0.f, 0.f, 0.f);
            leds.ui.Pth(9, 0.f, 0.f, 0.f);

            // 5mm leds
            leds.ui.Pth(5, 1.f, 1.f, 1.f);
            leds.ui.Pth(6, 1.f, 1.f, 1.f);
            leds.ui.Pth(7, 1.f, 1.f, 1.f);
            leds.ui.Pth(8, 1.f, 1.f, 1.f);
            leds.ui.ClearKeys();
            // leds.ui.Smt(i, .1f, .1f, .0f);
            // leds.ui.Smt(0, 0.f, 0.f, 0.f);
            // leds.ui.Smt(9, 0.f, 0.f, 0.f);
            // leds.ui.Smt(10, 0.f, 0.f, 0.f);
        }

        uint32_t last_arm_blink;
        bool arm_blink = true;

        uint32_t last_record_blink;
        bool record_blink = false;
        Hold rec_hold;

        void ResetSmtLeds()
        {
            leds.ui.ClearKeys();
        }

        // from options.json, once the card is read
        void SetOptions(uint8_t midi_out_channel, bool ps_quant, bool split_delay)
        {
            midi_channel = midi_out_channel;
            quantized_pitch_ = !ps_quant;
            split_delay_ = split_delay;
            if(split_delay_)
                enc_values[0][3] = .5f;
        }

        // from presets.json, once the card is read
        void ApplyPresets() { enc_values[0][5] = presets_->Volume(); }

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override __attribute__((optimize("Os")))
        {
            uint32_t now = System::GetNow();

            if(knob_page[0] == 2 && !fx_->ArpAllowed())
                knob_page[0] = 0;

            if(knob_page[0] == 2)
                arp_knob.Update(now);
            const Audition::Event audition_event = audition.Update(now);
            AuditionEvent(audition_event, audition.What());
            if(knob_page[3] == 0 && magic_hold.Held(now)) // the reverb page, where it shows
                grains.ToggleFreeze();

            if(rec_hold.Held(now))
                StartChompiRecording(0.f);
            if(fx_->TakeRecordingDone())
                RecordingDone();

            if(transport_hold.Held(now))
            {
                if(fx_->GetLooperIsEmpty())
                    beat_clock.NextBar();
                else if(!fx_->IsLooperFirstRecording()) // not while the first loop records
                    fx_->LooperMakeLengthBase();
                transport_flash = now;
            }

            // back to 1x, the wheel can't do it while the looper is empty
            if(fx_->CheckReset())
            {
                enc_values[0][4] = enc_defaults[0][4];
                hw_->SendCC(midi_channel, cc_map[0][4], enc_values[0][4] * 127.f);
                fx_->SetLooperPitch(1.f);
                fx_->ResetLooperPitchQuant();
            }

            // the switch up with a sample in RAM: START and END edit it, the keys show it
            const bool edit_keys = fx_->LoopEdit() || (!switch_state && fx_->EditInRam());
            int at_start = 0, at_end = 0, at_play = 0;
            bool on_transient = false, blink = false;
            const float *profile = nullptr;
            if(edit_keys)
            {
                at_start = std::min(int(kProfile) - 1, int(fx_->EditPoint(0) * kProfile));
                at_end = std::min(int(kProfile) - 1, int(fx_->EditPoint(1) * kProfile));
                at_play = int(fx_->EditPlayhead() * kProfile);
                on_transient = fx_->EditOnTransient();
                blink = (now / 250) % 2;
                profile = EditProfile();
            }
            for(size_t i = 7; i < (25 + 7); i++)
            {
                if(edit_keys) // the white keys: what's edited in bins, START's blinks yellow, END's red
                {
                    const int bin = WhiteKey(KeySlice(i));
                    if(bin < 0)
                    {
                        leds.ui.Smt(led_map[i], 0, 0, 0);
                        continue;
                    }
                    const float *c = white;
                    float v = .05f + .95f * profile[bin];
                    if(bin == at_start && on_transient)
                    {
                        c = dark_orange;
                        v = 1.f;
                    }
                    else if(bin == at_start && bin == at_end)
                    {
                        c = blink ? yellow : red;
                        v = 1.f;
                    }
                    else if((bin == at_start || bin == at_end) && blink)
                    {
                        c = bin == at_start ? yellow : red;
                        v = 1.f;
                    }
                    else if(bin == at_play && (blink || bin == at_start || bin == at_end))
                    {
                        c = blue;
                        v = 1.f;
                    }
                    leds.ui.Smt(led_map[i], c[0] * v, c[1] * v, c[2] * v);
                    continue;
                }
                size_t slot = KeyToSlot(i);
                const float* color = &pink[0];
                if(fx_->GetVoiceSlot() != 15 || fx_->GetVoiceMode() == VoiceMode::CUBBI)
                {
                    if(fx_->GetVoiceBank() == 0) // this shouldn't change if we're not actually on that bank
                        color = &purple[0];
                    else if(fx_->GetVoiceBank() == 1)
                        color = &orange[0];
                    else if(fx_->GetVoiceBank() == 2)
                        color = &teal[0];
                    else if(fx_->GetVoiceBank() == 3)
                        color = &dark_orange[0];
                    else if(fx_->GetVoiceBank() == 4)
                        color = &yellow_green[0];
                }

                const bool kit_leds = fx_->AiffSelected() && (switch_state || fx_->GetInputSource() != InputSource::MIC)
                                      && (fx_->KitSelected() || slot_loader.Busy() || slot_loader.Failed());
                if (fx_->IsKeyPlaying(i))
                    leds.ui.Smt(led_map[i], 1.f, 1.f, 1.f);
                else if(kit_leds) // loading blinks, red is a file it can't play, the slices light, the one to edit brighter
                {
                    const int slice = KeySlice(i);
                    if(slot_loader.Busy() && (System::GetNow() / 150) % 2)
                        leds.ui.Smt(led_map[i], color[0] * .25f, color[1] * .25f, color[2] * .25f);
                    else if(slot_loader.Busy())
                        leds.ui.Smt(led_map[i], 0, 0, 0);
                    else if(slot_loader.Failed())
                        leds.ui.Smt(led_map[i], red[0] * .25f, red[1] * .25f, red[2] * .25f);
                    else if(slice == slot_loader.LastSlice())
                        leds.ui.Smt(led_map[i], color[0] * .7f, color[1] * .7f, color[2] * .7f);
                    else if(slot_loader.HasSlice(slice))
                        leds.ui.Smt(led_map[i], color[0] * .25f, color[1] * .25f, color[2] * .25f);
                    else
                        leds.ui.Smt(led_map[i], 0, 0, 0);
                }
                else if(fx_->GetVoiceMode() == VoiceMode::CUBBI 
                        && (switch_state || fx_->GetInputSource() != InputSource::MIC)) // no perm KB leds if we're monitoring the mic
                {
                    if (fx_->GetFileExists(slot - 1) && i == 28)
                        leds.ui.Smt(led_map[i], pink[0] * .25f, pink[1] * .25f, pink[2] * .25f);
                    else if(fx_->GetFileExists(slot - 1) && slot != kSlotNone)
                        leds.ui.Smt(led_map[i], color[0] * .25f, color[1] * .25f, color[2] * .25f);
                }
                else if(fx_->GetVoiceMode() == VoiceMode::JAMMI && (i == 15 || i == 18 || i == 28)
                        && (switch_state || fx_->GetInputSource() != InputSource::MIC)) // no perm KB leds if we're monitoring the mic
                    leds.ui.Smt(led_map[i], color[0] * .25f, color[1] * .25f, color[2] * .25f);
                else
                    leds.ui.Smt(led_map[i], 0, 0, 0);
            }

            // =========   encoders   =========
            for (int i = 0; i < 6; i++)
            {
                uint8_t page = knob_page[i];
                float value = enc_values[page][i];
                if(fx_->LoopEdit() && (i == 0 || i == 3)) // pitch and the FX are out
                {
                    leds.ui.Pth(i + 1, 0.f, 0.f, 0.f);
                    continue;
                }
                if(i == 1 || i == 2) // the points: the sample's, the loop's in loop edit
                {
                    if(fx_->LoopEdit())
                        page = 0;
                    if(page == 0)
                        value = fx_->EditPoint(i - 1);
                }

                float r = 0.f; 
                float g = 0.f;
                float b = 0.f;
                switch (i)
                {
                case 0: // speed, gain, pan
                {
                    if(!switch_state) // recording threshold, red while the input is above it
                    {
                        const float t = enc_values[2][0];
                        r = g = b = t;
                        if(t > 0.f && fx_->GetVUSample(VUTarget::VU_INPUT) >= t * t * t)
                            g = b = 0.f;
                    }
                    else if(page == 0) // speed
                    {
                        speed_color(enc_values[0][0], r, g, b);
                    }
                    else if(page == 1) // gain
                    {
                        r = color_triple_xfade(blue[0], pink[0], red[0], value);
                        g = color_triple_xfade(blue[1], pink[1], red[1], value);
                        b = color_triple_xfade(blue[2], pink[2], red[2], value);

                        fx_->SetGain(value);
                    }
                    else
                        arp_knob.Led(false, now, r, g, b);

                    leds.ui.Pth(1, r, g, b);
                }
                break;
                case 1: // start point
                {
                    if (page == 0) // start point
                    {
                        r = color_xfade(yellow[0], orange[0], value);
                        g = color_xfade(yellow[1], orange[1], value);
                        b = color_xfade(yellow[2], orange[2], value);

                        // fx_->SetStartPoint(value);
                    }
                    else // env. attack
                    {
                        r = color_xfade(purple[0] * .2f, purple[0], value);
                        g = color_xfade(purple[1] * .2f, purple[1], value);
                        b = color_xfade(purple[2] * .2f, purple[2], value);

                        fx_->SetAttack(value);
                    }

                    if(!edit_keys && !switch_state)
                    {
                        r = g = b = 0.f;
                    }
                    if(fx_->EditOnTransient())
                        r = g = b = 1.f;

                    leds.ui.Pth(2, r, g, b);
                    break;
                }
                case 2: // end point
                {
                    if (page == 0) // end point
                    {
                        r = color_xfade(orange[0], red[0], value);
                        g = color_xfade(orange[1], red[1], value);
                        b = color_xfade(orange[2], red[2], value);

                        // fx_->SetEndPoint(value);
                    }
                    else // env. decay
                    {
                        r = color_xfade(purple[0] * .2f, purple[0], value);
                        g = color_xfade(purple[1] * .2f, purple[1], value);
                        b = color_xfade(purple[2] * .2f, purple[2], value);

                        fx_->SetDecay(value);
                    }

                    if(!edit_keys && !switch_state)
                    {
                        r = g = b = 0.f;
                    }

                    leds.ui.Pth(3, r, g, b);
                    break;
                }
                case 3: // magic
                {
                    if (page == 0) // reverb / delay
                    {   
                        // past 1: grains, the LED from blue to magenta
                        const float cloud = std::max(0.f, (value - 1.f) / (kMagicTop - 1.f));
                        const float rev = std::min(value, 1.f);
                        if (split_delay_) {
                            if (value < .5) {
                                fx_->SetReverbGrains(0.f, 0.f);
                                fx_->SetDelayFeedback((.5f - value) * 2.f);
                            }
                            else {
                                fx_->SetReverbGrains(std::min((value - .5f) * 2.f, 1.f), cloud);
                                fx_->SetDelayFeedback(0.f);
                            }

                            r = color_triple_xfade(green[0], (green[0] + blue[0]) * .5f, blue[0], rev);
                            g = color_triple_xfade(green[1], (green[1] + blue[1]) * .5f, blue[1], rev);
                            b = color_triple_xfade(green[2], (green[2] + blue[2]) * .5f, blue[2], rev);
                        }
                        else {
                            fx_->SetDelayFeedback(fx_->SetReverbGrains(rev, cloud));

                            r = color_triple_xfade(teal[0], med_blue[0], blue[0], rev);
                            g = color_triple_xfade(teal[1], med_blue[1], blue[1], rev);
                            b = color_triple_xfade(teal[2], med_blue[2], blue[2], rev);
                        }
                        if (cloud > 0.f) {
                            r = color_xfade(blue[0], magenta[0], cloud);
                            g = color_xfade(blue[1], magenta[1], cloud);
                            b = color_xfade(blue[2], magenta[2], cloud);
                        }
                        if (grains.Frozen() && (now / 400) % 2) { // blinks while frozen
                            r *= .3f;
                            g *= .3f;
                            b *= .3f;
                        }
                    }
                    else if (page == 1) // lofi
                    {
                        fx_->SetSaturate(value);
                        r = color_triple_xfade(yellow[0], orange[0], red[0], value);
                        g = color_triple_xfade(yellow[1], orange[1], red[1], value);
                        b = color_triple_xfade(yellow[2], orange[2], red[2], value);
                    }
                    else // filter
                    {
                        fx_->SetFilter(value);
                        r = color_triple_xfade(purple[0], pink[0], 1.f, value);
                        g = color_triple_xfade(purple[1], pink[1], 1.f, value);
                        b = color_triple_xfade(purple[2], pink[2], 1.f, value);
                    }

                    if(!switch_state)
                    {
                        r = g = b = 0.f;
                    }

                    leds.ui.Pth(4, r, g, b);

                    break;
                }
                case 4: // transport
                {
                    // no loop yet: the beats, white on the one
                    uint32_t beat;
                    float phase;
                    const bool beats = fx_->IsLooperFirstRecording() && beat_clock.Get(beat, phase);
                    if(fx_->GetLooperIsEmpty() || beats)
                    {
                        const float c = beats && phase < .25f ? (beat % beat_clock.BeatsPerBar() ? .3f : 1.f) : 0.f;
                        leds.ui.Pth(5, c, c, c);
                        leds.ui.Pth(6, c, c, c);
                    }
                    else if(fx_->GetTransportMode() != TransportMode::OFF)
                    {
                        static const float* const kLengthColors[] = {med_blue, green, yellow, orange, red};
                        const bool step = fx_->GetTransportMode() == TransportMode::STEP;
                        const int length_exp = fx_->GetLooperLengthExp();
                        const float* c = step ? purple : kLengthColors[std::min(abs(length_exp), 4)];

                        float ramp = 1.f;
                        if(!fx_->IsLooperFirstRecording())
                        {
                            const float pos = fx_->GetLooperPosition();
                            if(step)
                                ramp = (fx_->GetLooperSection() + pos) / (length_exp < 0 ? 1 << -length_exp : 1);
                            else
                                ramp = pos * (length_exp > 0 ? 1 << length_exp : 1);
                            ramp = 1.f - (ramp - static_cast<int>(ramp));
                        }
                        if(!switch_state)
                            ramp *= kRecDim;

                        float left = !step && length_exp > 0 ? 0.f : ramp;
                        float right = !step && length_exp < 0 ? 0.f : ramp;
                        if(now - transport_flash < 200)
                        {
                            c = white;
                            left = right = 1.f;
                        }
                        leds.ui.Pth(5, c[0] * left, c[1] * left, c[2] * left);
                        leds.ui.Pth(6, c[0] * right, c[1] * right, c[2] * right);
                    }
                    else if(fx_->IsLooperPlaying())
                    {
                        wheel_speed_leds(value, switch_state ? 1.f : kRecDim);
                    }
                    else
                    {
                        float scrub = fx_->GetLooperScrub() * .5f;
                        int led = 5 + (scrub > 0.f);

                        scrub = fabsf(scrub);
                        leds.ui.Pth(led, scrub, scrub, scrub);
                    }

                    break;
                }
                case 5: // gain
                {
                    if(batt_display && System::GetNow() - batt_hold > 2000)
                    {
                        const float* color = &green[0];

                        switch(hw_->GetBatteryLevel())
                        {
                            case Hardware::BatteryLevel::FULL:
                                color = &white[0];
                            break;
                            case Hardware::BatteryLevel::HIGH:
                                color = &green[0];
                            break;
                            case Hardware::BatteryLevel::MEDIUM:
                                color = &yellow[0];
                            break;
                            case Hardware::BatteryLevel::LOW:
                                color = &red[0];
                            break;
                            default:
                            break;
                        }

                        r = color[0];
                        g = color[1];
                        b = color[2];
                    }
                    else if (page == 0)
                    {
                        float vu_sample = fx_->GetVUSample(VUTarget::VU_OUTPUT);

                        r = value * color_quad_xfade(.1f, green[0], yellow[0], pink[0], vu_sample);
                        g = value * color_quad_xfade(.1f, green[1], yellow[1], pink[1], vu_sample);
                        b = value * color_quad_xfade(.1f, green[2], yellow[2], pink[2], vu_sample);

                        fx_->SetMainGain(value);
                    }
                    else
                    {
                        r = color_xfade(blue[0], red[0], value);
                        g = color_xfade(blue[1], red[1], value);
                        b = color_xfade(blue[2], red[2], value);

                        fx_->SetInputGain(value);
                    }
                    leds.ui.Pth(9, r, g, b);
                    break;
                }
                default:
                    break;
                }
            }

            /** PTH leds */
            float r, g, b;
            // play key
            if(fx_->GetLooperIsEmpty() && !fx_->IsLooperRecordArmed())
            {
                r = g = b = 0.f;
            }
            else if(fx_->IsLooperRecordArmed())
            {
                r = g = b = 1.f;
            }
            else if(fx_->IsLooperFirstRecording() && fx_->IsLooperRecording())
            {
                r = teal[0];
                g = teal[1];
                b = teal[2];
            }
            else if(fx_->IsLooperPlaying())
            {
                float position = 1.f - fx_->GetLooperPosition();
                r = teal[0] * position;
                g = teal[1] * position;
                b = teal[2] * position;
            }
            else // we're paused
            {
                float position = 1.f - fx_->GetLooperPosition();
                r = position;
                g = position;
                b = position;
            }

            if(!switch_state)
            {
                r *= kRecDim;
                g *= kRecDim;
                b *= kRecDim;
            }
            leds.ui.Pth(led_map[33], r, g, b);

            // loop key
            if(fx_->GetLooperIsEmpty() && !fx_->IsLooperRecordArmed())
            {
                r = g = b = 0.f;
            }
            else if(fx_->IsLooperRecordArmed())
            {
                if(now - last_arm_blink > 300)
                {
                    arm_blink = !arm_blink;
                    last_arm_blink = now;
                }

                r = arm_blink ? 1.f : 0.f;
                g = 0.f;
                b = 0.f;
            }
            else if(fx_->IsLooperFirstRecording() && fx_->IsLooperRecording())
            {
                r = red[0];
                g = red[1];
                b = red[2];
            }
            else if(fx_->IsLooperPrinting())
            {
                float position = fx_->GetLooperPosition();
                r = magenta[0] * position;
                g = magenta[1] * position;
                b = magenta[2] * position;
            }
            else if(fx_->IsLooperRecording()) // overdub
            {
                float position = fx_->GetLooperPosition();
                r = yellow[0] * position;
                g = yellow[1] * position;
                b = yellow[2] * position;
            }
            else
            {
                float position = fx_->GetLooperPosition();
                r = position;
                g = position;
                b = position;
            }

            if(!switch_state)
            {
                r *= kRecDim;
                g *= kRecDim;
                b *= kRecDim;
            }
            if(fx_->LoopEdit())
            {
                r = magenta[0];
                g = magenta[1];
                b = magenta[2];
            }
            leds.ui.Pth(led_map[34], r, g, b);

            // chompi key
            if (!switch_state)
            {
                if(copier_->IsCopying())
                {
                    if(now - last_record_blink > 300)
                    {
                        record_blink = !record_blink;
                        last_record_blink = now;
                    }

                    if(record_blink)
                    {
                        r = pink[0];
                        g = pink[1];
                        b = pink[2];
                    }
                    else
                    {
                        r = g = b = 0.f;
                    }
                }
                else if (fx_->Recording() ? !fx_->RecordEnding() : rec_hold.Down()) // red from the press on, off during the post-roll
                {
                    r = red[0];
                    g = red[1];
                    b = red[2];
                }
                else if(fx_->IsRecordWaiting() && (now / 250) % 2) // armed: dim red against the input level, 2 Hz
                {
                    r = .3f * red[0];
                    g = .3f * red[1];
                    b = .3f * red[2];
                }
                else
                {
                    float vu_sample = fx_->GetVUSample(VUTarget::VU_INPUT);
                
                    r = color_quad_xfade(.1f, green[0], yellow[0], pink[0], vu_sample);
                    g = color_quad_xfade(.1f, green[1], yellow[1], pink[1], vu_sample);
                    b = color_quad_xfade(.1f, green[2], yellow[2], pink[2], vu_sample);
                }
            }
            else
            {
                if (chompi_key_pressed)
                {
                    r = .67f;
                    g = 0.f;
                    b = 1.f;
                }
                else
                {
                    r = g = b = 0.f;
                }
            } 

            leds.ui.Pth(led_map[5], r, g, b);

            // ========   send the data   =========
            leds.Show();
        }

        bool OnButton(uint16_t buttonID,
                      uint8_t numberOfPresses,
                      bool isRetriggering) override
        {
            if (copier_->IsCopying())
            {
                OnFocusLost(); // drop the holds, their release may be swallowed here
                return false;
            }

            bool rising = numberOfPresses == 1;
            switch (buttonID)
            {
            // NO CONNECT, SKIP THESE
            case static_cast<uint16_t>(Hardware::SwId::NC_1): // fall through
            case static_cast<uint16_t>(Hardware::SwId::NC_2): // fall through
            case static_cast<uint16_t>(Hardware::SwId::NC_3): // fall through
            case static_cast<uint16_t>(Hardware::SwId::NC_4): // fall through
            case static_cast<uint16_t>(Hardware::SwId::NC_5): // fall through
                                                              // case static_cast<uint16_t>(Hardware::SwId::NC_6): // caught in ui.h
                break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_4_SW):
                if(arp_knob.Click(rising, System::GetNow()))
                    knob_page[0] = (knob_page[0] + 1) % (fx_->ArpAllowed() ? knob_num_pages[0] : 2);
                break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_3_SW): // hold: freeze the grains
                if(rising)
                    magic_hold.Press(System::GetNow());
                else if(magic_hold.Release())
                    knob_page[3] = (knob_page[3] + 1) % knob_num_pages[3];
                break;

            // encoder clicks, toggle pages
            case static_cast<uint16_t>(Hardware::SwId::ENC_1_SW): // fall through
            case static_cast<uint16_t>(Hardware::SwId::ENC_2_SW): // fall through
            {
                if(!rising)
                {
                    uint8_t knob = key_map[buttonID];
                    knob_page[knob]++;
                    knob_page[knob] %= knob_num_pages[knob];
                }
                break;
            }

            case static_cast<uint16_t>(Hardware::SwId::ENC_6_SW):
            {
                if(!rising && System::GetNow() - batt_hold < 2000)
                {
                    uint8_t knob = key_map[buttonID];
                    knob_page[knob]++;
                    knob_page[knob] %= knob_num_pages[knob];
                }

                batt_hold = System::GetNow();
                batt_display = rising;

                break;
            }

            case ENC_5_SW:
            {
                // no loop yet: click for the metronome, hold for the next time signature
                if(fx_->IsLooperFirstRecording())
                {
                    if(fx_->GetLooperIsEmpty())
                    {
                        if(rising)
                            transport_hold.Press(System::GetNow());
                        else if(transport_hold.Release())
                            beat_clock.SetMetronome(!beat_clock.Metronome());
                    }
                    break;
                }

                if(fx_->GetTransportMode() != TransportMode::OFF)
                {
                    if(rising)
                        transport_hold.Press(System::GetNow());
                    else if(transport_hold.Release())
                        fx_->LooperResetLength();
                    break;
                }

                if (!rising)
                {
                    enc_values[0][4] = enc_defaults[0][4];
                    hw_->SendCC(midi_channel, cc_map[0][4], enc_values[0][4] * 127.f);
                }

                fx_->SetLooperPitch(1.f);
                fx_->ResetLooperPitchQuant();
                break;
            }

            // toggle. We're not using this anymore, just here in case something breaks
            case static_cast<uint16_t>(Hardware::SwId::SW_TOG): // fall through
                // switch_state = rising;
                // if (!rising)
                //     midi_channel = 0;

                // some weirdness results in handling this on edges rather than as pressed
                // for example if you hold the chompi key with the switch up then toggle the sw
                // down, you'll be on ch 1 until you release and repress the chompi key
                // then it will go to ch 2 like it should
                break;

            // CC buttons
            // case static_cast<uint16_t>(Hardware::SwId::ENC_4_SW):
            // {
            //     if (!rising)
            //     {
            //         enc_values[0][0] = enc_defaults[0][0];
            //         hw_->SendCC(midi_channel, cc_map[0][0], enc_values[0][0] * 127);
            //         fx_->SetGlobalPitch(1.f);
            //         fx_->SetReverse(false);

            //         DumpValuePresets();
            //         leds.ui.Pth(1, green[0], green[1], green[2]);
            //     }

            //     hw_->SendCC(midi_channel, key_map[buttonID], rising ? 127 : 0);
            //     break;
            // }

            // CC buttons
            case static_cast<uint16_t>(Hardware::SwId::KEY_27): // play
            {
                last_arm_blink = System::GetNow();
                fx_->LooperPlayButton(rising);    
                hw_->SendCC(midi_channel, 26, rising ? 127 : 0);
                break;
            }
            case static_cast<uint16_t>(Hardware::SwId::KEY_28): // loop
            {
                last_arm_blink = System::GetNow();
                if(!fx_->Recording())
                    fx_->LooperRecordButton(rising);
                hw_->SendCC(midi_channel, 27, rising ? 127 : 0);
                break;
            }

            case static_cast<uint16_t>(Hardware::SwId::KEY_26):
            {
                chompi_key_pressed = rising;
                if (!switch_state)
                {
                    hw_->SendCC(midi_channel, key_map[buttonID], rising ? 127 : 0);
                }
                else
                {
                    midi_channel = rising;
                }

                // chompi mode: held, it records while held; a tap arms the threshold, or starts without one;
                // a tap while armed or recording stops
                if(!switch_state)
                {
                    if(rising && fx_->Recording())
                    {
                        fx_->StopRecording();
                        rec_hold.Cancel();
                    }
                    else if(rising)
                        rec_hold.Press(System::GetNow());
                    else if(rec_hold.Release())
                    {
                        if(fx_->IsRecordWaiting())
                            fx_->StopRecording();
                        else
                            StartChompiRecording(enc_values[2][0]);
                    }
                    else if(fx_->Recording())
                        fx_->StopRecording();
                }
 
                break;
            }

            // keys
            default:
                if(rising && fx_->LoopEdit()) // off, a release still ends its note
                    break;
                if (rising)
                {
                    // real keypress
                    if(!isRetriggering)
                    {
                        if(fx_->GetVoiceMode() == VoiceMode::CUBBI)
                        {
                            size_t slot = KeyToSlot(buttonID);
                            if(slot == kSlotNone)
                                return true;

                            OpenCubbiSlot(slot);
                        }

                        fx_->request_fifo.PushBack(KeyRequest(KeyRequest::Type::START, 
                            key_map[buttonID] - 60, buttonID, 127.f));
                    }

                    if(fx_->GetLooperRecordArm())
                        fx_->ToggleLooperRecord();

                    // this can take some time, so it must happen last
                    if(!isRetriggering)
                        hw_->SendNoteOn(midi_channel, key_map[buttonID], 127);
                }
                else
                {
                    if(!isRetriggering)
                    {
                        fx_->request_fifo.PushBack(KeyRequest(KeyRequest::Type::STOP, 
                            0, buttonID, 127.f));
                        hw_->SendNoteOff(midi_channel, key_map[buttonID], 127);
                    }
                }
                break;
            }

            return true;
        }

        void AuditionEvent(Audition::Event e, int what)
        {
            if(e != Audition::Event::kNone)
                fx_->AuditionPoint(Engine::Point(what), e == Audition::Event::kStart);
        }

        // A point moved: its audition starts once the knob rests.
        void AuditionTurn(Engine::Point p)
        {
            if(audition.On())
                AuditionEvent(Audition::Event::kStop, audition.What());
            audition.Turn(int(p), System::GetNow(), fx_->AuditionMs(p));
        }

        void OpenCubbiSlot(size_t slot)
        {
            if(!fx_->GetFileExists(slot - 1))
                return; // no file in that slot

            size_t mode = static_cast<size_t>(fx_->GetVoiceMode());
            size_t bank = fx_->GetBank();

            bool loop = true;
            bool sustain = true;
            float pan = .5f;

            if(!presets_->IsValid(mode, bank, slot))
            {
                enc_values[0][0] = enc_defaults[0][0];
                enc_values[0][1] = enc_defaults[0][1];
                enc_values[0][2] = enc_defaults[0][2];
                enc_values[1][0] = enc_defaults[1][0];
                enc_values[1][1] = enc_defaults[1][1];
                enc_values[1][2] = enc_defaults[1][2];
            }
            else
            {
                /** TODO: this scheme is bad now. Either we mess up the order, or we don't match the old scheme's slots */
                enc_values[0][0] = presets_->GetValue(mode, bank, slot, 0);
                enc_values[0][1] = presets_->GetValue(mode, bank, slot, 1);
                enc_values[0][2] = presets_->GetValue(mode, bank, slot, 2);
                enc_values[1][1] = presets_->GetValue(mode, bank, slot, 3);
                enc_values[1][2] = presets_->GetValue(mode, bank, slot, 4);
                loop = presets_->GetValue(mode, bank, slot, 5);
                sustain = presets_->GetValue(mode, bank, slot, 6);

                enc_values[1][0] = presets_->GetValue(mode, bank, slot, 7);
                pan = presets_->GetValue(mode, bank, slot, 8);
            }

            // TODO: clean up pitch code repetition
            float val = enc_values[0][0];
            val = val < .5f ? (.5f - val) * -2.f : (val - .5f) * 2.f; // 1 - 0 - 1

            float inv = val < 0.f ? -1.f : 1.f;
            float pitch;
            if(fabsf(val) < .33f) // .01x - .5x
                pitch = val * 1.484848f + .01f * inv;
            else if (fabsf(val) < .66f ) // .5x - 1x
                pitch = (val - .33f * inv) * 1.515151 + .5f * inv;
            else // 1x - 2x
                pitch = (val - .66 * inv) * 2.941176 + 1.f * inv;

            fx_->OpenCubbiSlot(pitch, enc_values[0][1], enc_values[0][2], enc_values[1][1], 
            enc_values[1][2], loop, sustain, enc_values[1][0],
            pan);
        }

        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            if (copier_->IsCopying())
                return false;

            // sampling: the left encoder sets the recording threshold, off all the way down
            if(encoderID == 0 && !switch_state && stepsPerRevolution == 0)
            {
                enc_values[2][0] = fclamp(enc_values[2][0] + turns * kEncoderCoarseStep, 0.f, 1.f);
                hw_->SendCC(midi_channel, kThresholdCC, enc_values[2][0] * 127);
                return true;
            }

            // no loop yet: tempo, locked while the first loop records
            if(encoderID == 4 && stepsPerRevolution == 0 && fx_->IsLooperFirstRecording())
            {
                if(fx_->GetLooperIsEmpty())
                    beat_clock.ChangeTempo(turns);
                return true;
            }

            const TransportMode transport = fx_->GetTransportMode();
            if(encoderID == 4 && transport != TransportMode::OFF && stepsPerRevolution == 0)
            {
                transport_turns += turns > 0 ? 1 : -1;
                if(transport_turns == 2 || transport_turns == -2)
                {
                    if(transport == TransportMode::STEP)
                        fx_->LooperStep(transport_turns / 2);
                    else
                        fx_->LooperLengthChange(transport_turns / 2);
                    transport_turns = 0;
                }
                return true;
            }

            uint8_t page = knob_page[encoderID];
            float old_val = enc_values[page][encoderID];
            const float top = encoderID == 3 && page == 0 ? kMagicTop : 1.f;
            if(encoderID == 3) // turned while pressed: no click, no freeze
                magic_hold.Cancel();

            // START and END on the sample page: a CC sets the sample's point as a fraction. A detent moves the last
            // played slice of a kit, else the point of what is edited, which the engine keeps; an event is one
            // detent, its turns carry the knob pages' scale.
            if(page == 0 && (encoderID == 1 || encoderID == 2))
            {
                const int k = encoderID - 1;
                if(stepsPerRevolution > 0)
                {
                    const float f = turns / 127.f;
                    if(k ? fx_->SetEndPoint(f) : fx_->SetStartPoint(f))
                        DumpValuePresets();
                    return true;
                }
                if(!fx_->LoopEdit() && fx_->EditKit(k == 1, turns))
                    return true;
                if(!fx_->MovePoint(k, turns < 0 ? -1 : 1))
                    return true;
                AuditionTurn(k ? Engine::Point::kEnd : Engine::Point::kStart);
                if(!fx_->LoopEdit())
                {
                    hw_->SendCC(midi_channel, cc_map[0][encoderID], fx_->SamplePoint(k) * 127);
                    DumpValuePresets();
                }
                return true;
            }

            // no CC: enc_values[2][0] is the threshold
            if(encoderID == 0 && page == 2)
            {
                if(stepsPerRevolution == 0)
                    arp_knob.Turn(turns, false);
                return true;
            }

            // overrode this to mean increment vs force knob position (used for CCs)
            if(stepsPerRevolution > 0)
            {
                enc_values[page][encoderID] = turns * top / 127.f;
            }
            else{
                float inc = turns * kEncoderCoarseStep;

                if((encoderID == 0 && page == 0 && quantized_pitch_)
                    || (encoderID == 4 && quantized_pitch_))
                {
                    inc = 0.f;
                }
                else if ((encoderID == 0 && page == 0 && !quantized_pitch_)
                    || (encoderID == 4 && !quantized_pitch_))
                {
                    inc = turns * kEncoderFineStep;
                }

                enc_values[page][encoderID] += inc;
            }

            // clip
            enc_values[page][encoderID] = fclamp(enc_values[page][encoderID], 0.f, top);

            if (encoderID == 0 && page == 0)
            {
                if(quantized_pitch_)
                    enc_values[0][0] = fx_->SetGlobalPitchQuantized(turns, enc_values[0][0]);
                else
                    fx_->SetGlobalPitchFree(enc_values[0][0]);
            }
            else if (encoderID == 4)
            {
                if (fx_->IsLooperPlaying())
                {
                    if(quantized_pitch_)
                        enc_values[0][4] = fx_->SetLooperPitchQuantized(turns, enc_values[0][4]);
                    else
                        fx_->SetLooperPitchFree(enc_values[0][4]);
                }
                else
                {
                    enc_values[0][4] = old_val;
                    fx_->SetLooperScrub(turns);
                }  
            }

            if (stepsPerRevolution == 0) {
                hw_->SendCC(midi_channel, cc_map[page][encoderID], enc_values[page][encoderID] / top * 127);
            }


            if(encoderID < 3)
            {
                DumpValuePresets();
            }
            else if(encoderID == 5 && page == 0)
                presets_->SetVolume(enc_values[0][5]);

            return true;
        }

        void DumpValuePresets()
        {
            size_t mode = static_cast<size_t>(fx_->GetVoiceMode());
            size_t bank = fx_->GetBank();
            size_t slot = fx_->GetVoiceSlot();

            presets_->SetValue(enc_values[0][0], mode, bank, slot, 0);
            presets_->SetValue(enc_values[0][1], mode, bank, slot, 1);
            presets_->SetValue(enc_values[0][2], mode, bank, slot, 2);
            presets_->SetValue(enc_values[1][1], mode, bank, slot, 3);
            presets_->SetValue(enc_values[1][2], mode, bank, slot, 4);
            presets_->SetValue(fx_->GetAutoLoop(), mode, bank, slot, 5);
            presets_->SetValue(fx_->GetSustainActive(), mode, bank, slot, 6);

            presets_->SetValue(enc_values[1][0], mode, bank, slot, 7);
            presets_->SetValue(fx_->GetPan(), mode, bank, slot, 8);

        }

        void SetSwitchState(bool state)
        {
            if(state == switch_state)
                return;
            if(state && (fx_->Recording() || fx_->IsRecordWaiting()) && !copier_->IsCopying())
                fx_->StopRecording();
            switch_state = state;
            fx_->SetSwitch(!state);
        }

        void StartChompiRecording(float threshold)
        {
            fx_->StartNewRecording(threshold * threshold * threshold);
            last_record_blink = System::GetNow();
            record_blink = false;
        }

        // the points on the recording itself, its pre- and post-roll outside
        void RecordingDone()
        {
            enc_values[0][0] = enc_defaults[0][0];
            enc_values[1][0] = enc_defaults[1][0];
            enc_values[1][1] = enc_defaults[1][1];
            enc_values[1][2] = enc_defaults[1][2];
            const double frames = fx_->RecordFrames();
            fx_->SetStartPointForce(frames ? (fx_->RecordStart() + .5) / frames : 0.f);
            fx_->SetEndPointForce(frames ? std::min(1., (frames - fx_->RecordPost() + .5) / frames) : 1.f);
            DumpValuePresets();
        }

        // the release went elsewhere: no click, no hold
        void OnFocusLost() override
        {
            transport_hold.Lost();
            magic_hold.Lost();
            arp_knob.Lost();
            if(audition.On())
                AuditionEvent(Audition::Event::kStop, audition.What());
            audition.Turn(0, System::GetNow());
        }

    private:
        // The keys show what is edited in bins, measured when they are asked for memory they haven't seen.
        static constexpr size_t kProfile = 15;
        const float *EditProfile()
        {
            if(!fx_->EditInRam())
                return profile_;
            const Engine::Region r = fx_->EditRegion();
            const bool loop_edit = fx_->LoopEdit();
            if(r.mem != profile_mem_ || r.begin != profile_begin_ || r.end != profile_end_ || loop_edit != profile_loop_edit_)
            {
                profile_mem_ = r.mem;
                profile_begin_ = r.begin;
                profile_end_ = r.end;
                profile_loop_edit_ = loop_edit;
                chompi::Profile(r.mem, r.begin, r.end, profile_, kProfile);
            }
            return profile_;
        }

        static int KeySlice(size_t key) { return key_map[key] - 48; } // as StartPlayback picks it
        // a white key's number from the left, -1 for a black one
        static int WhiteKey(int note)
        {
            static const int8_t kWhite[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};
            const int w = kWhite[note % 12];
            return w < 0 ? -1 : note / 12 * 7 + w;
        }

        Hardware *hw_;
        Engine *fx_;
        FileCopier *copier_;
        PresetManager* presets_;

        float** enc_values;
        const float** enc_defaults;

        /** todo: these really shouldn't be stored in here
         *  Gonna move them out to a midi engine later
        */ 
        uint8_t midi_channel = 0;
        bool switch_state = false;
        bool chompi_key_pressed = false;
        Audition audition;
        int8_t transport_turns = 0;
        Hold transport_hold, magic_hold;
        uint32_t transport_flash = 0;
        uint8_t* knob_page;
        bool quantized_pitch_ = false;
        bool split_delay_ = false;
        float profile_[kProfile] = {};
        const int16_t *profile_mem_ = nullptr;
        size_t profile_begin_ = 0, profile_end_ = 0;
        bool profile_loop_edit_ = false;

        bool batt_display;
        uint32_t batt_hold;

    };

} // namespace chompi
