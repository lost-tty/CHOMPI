#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#include "hardware.h"
#include "DSPEngine.h"
#include "KnobPreset.h"
#include "Leds.h"
#include "FileCopier.h"

namespace chompi
{
    class MenuPage : public daisy::UiPage
    {
    public:

        enum class PresetMode
        {
            NONE = 0,
            ERASE_SEL,
            ERASING,
            COPY_SRC,
            COPY_DEST,
            COPYING,
            SAVE_SEL,
            SAVING,
            LAST,
        };


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

            key_color = &purple[0];

            last_blink = System::GetNow();

            chompi_key_pressed = false;

            preset_mode = PresetMode::NONE;

            input_toggled = false;

            final_comp = 0.f;
            resonance = 0.f;
            warble = 0.f;
            fx_->SetFinalComp(final_comp);
            fx_->SetFilterResonance(resonance);
            fx_->SetDelayTime(.5f);
            fx_->SetWarble(warble);

            ss_bank = fx_->GetBank();
        }

        void Draw(const daisy::UiCanvasDescriptor &canvasDescriptor) override
        {
            /** PTH leds */
            float r, g, b;
            uint32_t now = System::GetNow();

            if(now - last_blink > 250)
            {
                blink_state = !blink_state;
                last_blink = now;
            }

            if(long_press_key != kNoKey && now - long_press_down >= kLongPress)
            {
                if(long_press_key == static_cast<uint16_t>(Hardware::SwId::KEY_19))
                    fx_->CycleLineMode();
                else if(long_press_key == static_cast<uint16_t>(Hardware::SwId::KEY_18))
                    // mic held: USB in and back
                    fx_->PickInputSource(was_usb ? InputSource::MIC : InputSource::USB);
                else if(long_press_key == static_cast<uint16_t>(Hardware::SwId::KEY_27))
                {
                    undo_ok = !copier_->IsCopying() && fx_->LooperUndo();
                    undo_flash = now;
                }
                else if(long_press_key == static_cast<uint16_t>(Hardware::SwId::KEY_24)) // copy held: the card as a USB drive
                {
                    preset_mode = PresetMode::NONE;
                    card_mode_request_ = true;
                }
                else if(copier_->IsCopying())
                    bake_refused = now;
                else
                    fx_->BakeFx();
                long_press_key = kNoKey;
            }
            if(fx_->TakeBakeRefused())
                bake_refused = now;

            // chompi key
            if (chompi_key_pressed && preset_mode == PresetMode::NONE)
            {
                r = .67f;
                g = 0.f;
                b = 1.f;
            }
            else if (
                    (preset_mode == PresetMode::SAVE_SEL
                    || preset_mode == PresetMode::ERASE_SEL
                    || preset_mode == PresetMode::COPY_DEST)
                    && selected_slot != kSlotNone)
            {
                r = blink_state;
                g = b = 0.f;
            }
            else if (preset_mode == PresetMode::SAVING 
                        || preset_mode == PresetMode::COPYING 
                        || preset_mode == PresetMode::ERASING)
            {
                r = g = b = blink_state;
            }
            else
            {
                r = g = b = 0.f;
            }
            leds.ui.Pth(0, r, g, b);
        
            // play / overdub keys
            if(preset_mode == PresetMode::NONE)
            {
                const float gain = fx_->GetLooperDubGain();
                if(now - undo_flash < 300)
                    leds.ui.Pth(7, undo_ok ? 0.f : 1.f, undo_ok ? 1.f : 0.f, 0.f);
                else
                    leds.ui.Pth(7, gain, gain, gain);

                if(fx_->IsLooperPrinting())
                    leds.ui.Pth(8, magenta[0], magenta[1], magenta[2]);
                else if(now - bake_refused < 300)
                    leds.ui.Pth(8, red[0], red[1], red[2]);
                else
                    leds.ui.Pth(8, gain, gain, gain);

                // the wheel shows the transport mode SHIFT + click switches to, the pitch once it turns
                if(wheel_turned)
                {
                    wheel_speed_leds(enc_values[0][4], switch_state ? 1.f : kRecDim);
                }
                else
                {
                    const TransportMode mode = fx_->GetTransportMode();
                    const float* c = mode == TransportMode::LENGTH ? med_blue : mode == TransportMode::STEP ? purple : green;
                    leds.ui.Pth(5, c[0], c[1], c[2]);
                    leds.ui.Pth(6, c[0], c[1], c[2]);
                }
            }

            else if (preset_mode == PresetMode::COPY_SRC
                || preset_mode == PresetMode::COPY_DEST)
            {
                leds.ui.Pth(5, 0.f, 0.f, 0.f);
                leds.ui.Pth(6, 0.f, 0.f, 0.f);                

                if(selected_slot == 16)
                {
                    leds.ui.Pth(looper_append ? 8 : 7, blue[0], blue[1], blue[2]);
                    leds.ui.Pth(looper_append ? 7 : 8, 0.f, 0.f, 0.f);
                }
                else if(copy_src == 16)
                {
                    leds.ui.Pth(7, green[0], green[1], green[2]);
                    leds.ui.Pth(8, green[0], green[1], green[2]);
                }
                else if(!blink_state)
                {
                    leds.ui.Pth(7, 0.f, 0.f, 0.f);
                    leds.ui.Pth(8, 0.f, 0.f, 0.f);
                }
                else if(fx_->GetLooperIsEmpty() && preset_mode == PresetMode::COPY_DEST)
                {
                    leds.ui.Pth(7, .4f, .4f, .4f);
                    leds.ui.Pth(8, .4f, .4f, .4f);

                }
                else if(!fx_->GetLooperIsEmpty())
                {
                    leds.ui.Pth(7, .4f * pink[0], .4f * pink[1], .4f * pink[2]);
                    leds.ui.Pth(8, .4f * pink[0], .4f * pink[1], .4f * pink[2]);
                }
                // unselected, invalid destination
                else
                {
                    leds.ui.Pth(7, 0.f, 0.f, 0.f);
                    leds.ui.Pth(8, 0.f, 0.f, 0.f);
                }
            }
            else
            {
                leds.ui.Pth(5, 0.f, 0.f, 0.f);
                leds.ui.Pth(6, 0.f, 0.f, 0.f);                

                leds.ui.Pth(7, 0.f, 0.f, 0.f);
                leds.ui.Pth(8, 0.f, 0.f, 0.f);
            }

            // shift encoder display
            if(chompi_key_pressed)
            {
                // FX
                if(fx_reset)
                {
                    leds.ui.Pth(4, 1.f, 1.f, 1.f);
                }
                else if(knob_page[3] == 0) // magic
                {
                    static const float* const kNoteColors[] = {red, orange, yellow, green, med_blue, purple};
                    const int note = beat_clock.DelayNote();
                    const float knob = fx_->DelayKnob();
                    if(note >= 0 && fx_->DelayQuarter() > 0.f)
                        leds.ui.Pth(4, kNoteColors[note][0], kNoteColors[note][1], kNoteColors[note][2]);
                    else
                        leds.ui.Pth(4, knob, knob, knob);
                }
                else if(knob_page[3] == 1) // lofi
                {
                    leds.ui.Pth(4, warble, warble, warble);
                }
                else // filter
                {
                    leds.ui.Pth(4, resonance, resonance, resonance);
                }

                if(input_toggled)
                {
                    switch(fx_->GetMonitorMode())
                    {
                        case MonitorMode::BOTH:
                            r = blue[0];
                            g = blue[1];
                            b = blue[2];
                            break;
                        case MonitorMode::HP:
                            r = orange[0];
                            g = orange[1];
                            b = orange[2];
                            break;
                        case MonitorMode::SEND_RET:
                        default:
                            r = yellow[0];
                            g = yellow[1];
                            b = yellow[2];
                            break;
                    }
                }
                else
                {
                    // headphone os gain
                    // float idx = final_comp;

                    r = med_blue[0] * (final_comp * .9f + .1f);
                    g = med_blue[1] * (final_comp * .9f + .1f);
                    b = med_blue[2] * (final_comp * .9f + .1f);

                    // r = color_xfade(yellow[0], purple[0], final_comp);
                    // g = color_xfade(yellow[1], purple[1], final_comp);
                    // b = color_xfade(yellow[2], purple[2], final_comp);
                }

                leds.ui.Pth(9, r, g, b);

                // pitch knob
                if(pitch_reset)
                {
                    r = g = b = 1.f;
                }
                else if(knob_page[0] == 0)
                {
                    speed_color(enc_values[0][0], r, g, b);
                }
                else if(knob_page[0] == 1)
                {
                    r = color_triple_xfade(purple[0], 0.f, yellow[0], fx_->GetPan());
                    g = color_triple_xfade(purple[1], 0.f, yellow[1], fx_->GetPan());
                    b = color_triple_xfade(purple[2], 0.f, yellow[2], fx_->GetPan());
                }
                else
                    arp_knob.Led(true, false, System::GetNow(), r, g, b);

                leds.ui.Pth(1, r, g, b);
            }

            // save key
            if((preset_mode == PresetMode::SAVE_SEL 
                || preset_mode == PresetMode::SAVING
                || preset_mode == PresetMode::NONE)
                && !no_sd_card_)
            {
                if(fx_->GetFileExists(14))
                    leds.ui.Smt(9, blue[0], blue[1], blue[2]);
                else
                    leds.ui.Smt(9, .1f, .1f, .1f);
    
            }
            else
            {
                leds.ui.Smt(9, 0.f, 0.f, 0.f);
            }

            // copy key
            if((preset_mode == PresetMode::COPY_SRC 
                || preset_mode == PresetMode::COPY_DEST
                || preset_mode == PresetMode::COPYING
                || preset_mode == PresetMode::NONE)
                && !no_sd_card_)
            {
                leds.ui.Smt(8, green[0], green[1], green[2]); // copy
            }
            else
            {
                leds.ui.Smt(8, 0.f, 0.f, 0.f);
            }

            // erase key
            if((preset_mode == PresetMode::ERASE_SEL 
                || preset_mode == PresetMode::ERASING
                || preset_mode == PresetMode::NONE)
                && !no_sd_card_)
            {
                leds.ui.Smt(7, red[0], red[1], red[2]); // erase
            }
            else
            {
                leds.ui.Smt(7, 0.f, 0.f, 0.f);
            }

            // FX pre / post looper
            int led_sel = fx_->GetFxPreLooper() ? 5 : 6;
            int led_off = fx_->GetFxPreLooper() ? 6 : 5;
            leds.ui.Smt(led_sel, yellow[0], yellow[1], yellow[2]);
            leds.ui.Smt(led_off, 0.f, 0.f, 0.f);
        
            // Input select, USB blue on the mic LED
            const InputSource source = fx_->GetInputSource();
            led_sel = source == InputSource::USB ? 2 : 2 + static_cast<int>(source);
            leds.ui.Smt(2, 0.f, 0.f, 0.f);
            leds.ui.Smt(3, 0.f, 0.f, 0.f);
            leds.ui.Smt(4, 0.f, 0.f, 0.f);
            const LineMode line = source == InputSource::LINE_IN ? fx_->GetLineMode() : LineMode::STEREO;
            if(source == InputSource::USB)
                leds.ui.Smt(led_sel, blue[0], blue[1], blue[2]);
            else if(line == LineMode::MONO)
                leds.ui.Smt(led_sel, red[0], red[1], red[2]);
            else if(line == LineMode::MONO_RIGHT)
                leds.ui.Smt(led_sel, green[0], green[1], green[2]);
            else
                leds.ui.Smt(led_sel, pink[0], .7f * pink[1], .7f * pink[2]);

            // white keys
            for (uint8_t i = 1; i < 16; i++)
            {
                if(no_sd_card_)
                    leds.ui.Smt(25 - i, red[0], red[1], red[2]);
                else if(card_mode_)
                    leds.ui.Smt(25 - i, blue[0], blue[1], blue[2]);
                else if(preset_mode == PresetMode::SAVE_SEL && i == 15)
                    leds.ui.Smt(25 - i, pink[0], pink[1], pink[2]);
                else if(ss_bank == fx_->GetBank() && selected_slot == i && preset_mode == PresetMode::SAVE_SEL && fx_->GetVoiceMode() == ss_mode)
                    leds.ui.Smt(25 - i, blue[0], blue[1], blue[2]);
                else if(ss_bank == fx_->GetBank() && selected_slot == i && preset_mode == PresetMode::ERASE_SEL && fx_->GetVoiceMode() == ss_mode)
                    leds.ui.Smt(25 - i, red[0], red[1], red[2]);
                else if(ss_bank == fx_->GetBank() && selected_slot == i && preset_mode == PresetMode::COPY_DEST && fx_->GetVoiceMode() == ss_mode)
                    leds.ui.Smt(25 - i, blue[0], blue[1], blue[2]);
                else if( (ss_bank == fx_->GetBank() && selected_slot == i && preset_mode == PresetMode::COPY_SRC && fx_->GetVoiceMode() == ss_mode)
                            || (cs_bank == fx_->GetBank() && copy_src == i && preset_mode == PresetMode::COPY_DEST && fx_->GetVoiceMode() == cs_mode) )
                    leds.ui.Smt(25 - i, green[0], green[1], green[2]);
                else if( ((size_t(fx_->GetBank()) == fx_->GetVoiceBank() && fx_->GetVoiceSlot() == i) || (fx_->GetVoiceSlot() == 15 && i == 15))
                        && preset_mode == PresetMode::NONE 
                        && fx_->GetVoiceMode() == VoiceMode::JAMMI)
                    leds.ui.Smt(25 - i, 1.f, 1.f, 1.f);
                else if (
                    preset_mode == PresetMode::SAVE_SEL 
                    || preset_mode == PresetMode::ERASE_SEL
                    || preset_mode == PresetMode::COPY_SRC
                    || preset_mode == PresetMode::COPY_DEST
                )
                {
                    if (!blink_state)
                        leds.ui.Smt(25 - i, 0.f, 0.f, 0.f);
                    else if(preset_mode == PresetMode::ERASE_SEL && i == 15)
                        leds.ui.Smt(25 - i, 0.f, 0.f, 0.f);
                    else if(i == 15 && fx_->GetFileExists(i - 1))
                        leds.ui.Smt(25 - i, .4f * pink[0], .4f * pink[1], .4f * pink[2]);
                    else if(fx_->GetFileExists(i - 1))
                        leds.ui.Smt(25 - i, .4f * key_color[0], .4f * key_color[1], .4f * key_color[2]);
                    else if (
                        preset_mode == PresetMode::SAVE_SEL
                        || preset_mode == PresetMode::COPY_DEST
                    )
                    {
                        leds.ui.Smt(25 - i, .4f, .4f, .4f);
                    }
                    else
                        leds.ui.Smt(25 - i, 0.f, 0.f, 0.f);
                }
                else if(i == 15 && fx_->GetFileExists(i - 1))
                        leds.ui.Smt(25 - i, pink[0], pink[1], pink[2]);
                else if(fx_->GetFileExists(i - 1))
                {
                    const float k = Engine::IsKitSlot(i, fx_->GetBank(), fx_->GetVoiceMode()) ? .3f : 1.f; // a kit dim
                    leds.ui.Smt(25 - i, k * key_color[0], k * key_color[1], k * key_color[2]);
                }
                else
                    leds.ui.Smt(25 - i, 0.f, 0.f, 0.f);
            }

            // banks
            int mode = int(fx_->GetVoiceMode());
            leds.ui.Smt(mode == 0 ? 1 : 0, 0.f, 0.f, 0.f);
            switch(fx_->GetBank())
            {
                case 0:
                    key_color = &purple[0];
                    break;
                case 1:
                    key_color = &orange[0];
                    break;
                case 2:
                    key_color = &teal[0];
                    break;
                case 3:
                    key_color = &dark_orange[0];
                    break;
              default:
                    key_color = &yellow_green[0];
            }                

            if(!no_sd_card_)
                leds.ui.Smt(mode, key_color[0], key_color[1], key_color[2]);
            else
                leds.ui.Smt(mode, 0.f, 0.f, 0.f);

            // ========   send the data   =========
            leds.Show();
        }


        bool OnEncoderTurned(uint16_t encoderID,
                             int16_t turns,
                             uint16_t stepsPerRevolution) override
        {
            uint8_t page = knob_page[encoderID];
            float inc = turns * kEncoderCoarseStep;

            // we're receiving a knob position via CC
            if(stepsPerRevolution > 0)
                return false; // fall through to normalpage

            float r, g, b;
            if(preset_mode == PresetMode::NONE)
            {
                if(encoderID == 0)
                {
                    if(page == 0) // stepped pitch
                    {
                        if(quantized_pitch_)
                            enc_values[0][0] = fx_->SetGlobalPitchQuantized(turns, enc_values[0][0]);                                
                        else
                        {
                            enc_values[page][encoderID] += turns * kEncoderFineStep;
                            enc_values[page][encoderID] = fclamp(enc_values[page][encoderID], 0.f, 1.f);
                            fx_->SetGlobalPitchFree(enc_values[0][0]);
                        } 
                    }
                    else if(page == 1) // pan
                    {
                        fx_->SetPan(fx_->GetPan() + inc);
                    }
                    else
                        arp_knob.Turn(turns, true, false);

                    DumpValuePresets();
                }
                // move sample window (both start and end pos)
                else if ( (encoderID == 1 && page == 0)
                         || (encoderID == 2 && page == 0)
                )
                {
                    float start = fx_->SamplePoint(0), end = fx_->SamplePoint(1);
                    // positive and negative get blocked from moving out of bounds
                    if( !(turns > 0 && end + inc > 1.f) // positive clip
                        && !(turns < 0 && start + inc < 0.f) ) // negative clip
                    {
                        start += inc;
                        end += inc;
                        fx_->SetStartPointForce(start);
                        fx_->SetEndPointForce(end);

                        DumpValuePresets();
                    }

                    // start point
                    r = color_xfade(yellow[0], orange[0], start);
                    g = color_xfade(yellow[1], orange[1], start);
                    b = color_xfade(yellow[2], orange[2], start);
                    leds.ui.Pth(2, r, g, b);

                    // end point
                    r = color_xfade(orange[0], red[0], end);
                    g = color_xfade(orange[1], red[1], end);
                    b = color_xfade(orange[2], red[2], end);
                    leds.ui.Pth(3, r, g, b);
                    knob_page[1] = knob_page[2] = 0;
                }
                // set both attack and decay at once
                else if ( (encoderID == 1 && page == 1)
                         || (encoderID == 2 && page == 1)
                )
                {
                    float val = encoderID == 1 ? enc_values[1][1] : enc_values[1][2];
                    val = fclamp(val + inc, 0.f, 1.f);
                    fx_->SetAttack(val);
                    fx_->SetDecay(val);

                    DumpValuePresets();

                    // att.
                    r = color_xfade(purple[0] * .2f, purple[0], val);
                    g = color_xfade(purple[1] * .2f, purple[1], val);
                    b = color_xfade(purple[2] * .2f, purple[2], val);
                    leds.ui.Pth(2, r, g, b);
                    leds.ui.Pth(3, r, g, b);

                    enc_values[1][1] = val;
                    enc_values[1][2] = val;
                    knob_page[1] = knob_page[2] = 1;
                }
                else if (encoderID == 3)
                {
                    if(page == 0) // magic
                    {
                        beat_clock.TouchDelay();
                        if(!fx_->StepDelayNote(turns))
                        {
                            fx_->SetDelayTime(fclamp(fx_->DelayKnob() + inc, 0.f, 1.f));
                        }
                    }
                    else if (page == 1) // lofi
                    {
                        warble += inc;
                        warble = fclamp(warble, 0.f, 1.f);
                        fx_->SetWarble(warble);
                    }
                    else if(page == 2) // filter
                    {
                        resonance += inc;
                        resonance = fclamp(resonance, 0.f, 1.f);
                        fx_->SetFilterResonance(resonance);
                    }
                }
                else if(encoderID == 4)
                {
                    if(quantized_pitch_)
                        enc_values[0][4] = fx_->SetLooperPitchQuantized(turns, enc_values[0][4]);
                    else
                    {
                        enc_values[0][4] += turns * kEncoderFineStep;
                        enc_values[0][4] = fclamp(enc_values[0][4], 0.f, 1.f);
                        fx_->SetLooperPitchFree(enc_values[0][4]);
                    }
                    wheel_turned = true;
                }               
                else if(encoderID == 5)
                {
                    final_comp += inc;
                    final_comp = fclamp(final_comp, 0.f, 1.f);
                    fx_->SetFinalComp(final_comp);
                    input_toggled = false;
                }
            }


            return true;
        }

        void DumpValuePresets()
        {
            size_t mode = static_cast<size_t>(fx_->GetVoiceMode());
            size_t bank = fx_->GetBank();
            size_t slot = fx_->GetVoiceSlot();

            presets_->Store(mode, bank, slot, PresetFromKnobs(enc_values, *fx_));
        }

        void SetVoiceSlot(size_t slot)
        {
            // fx_->SetVoiceSlot(slot, true);

            size_t mode = static_cast<size_t>(fx_->GetVoiceMode());
            size_t bank = fx_->GetBank();

            const PresetManager::Preset p = presets_->Recall(mode, bank, slot);
            KnobsFromPreset(p, enc_values);
            fx_->ApplyPreset(p);
            fx_->SetVoiceSlot(slot, true);
        }

        bool OnButton(uint16_t buttonID,
                uint8_t numberOfPresses,
                bool isRetriggering) override
        {
            if(isRetriggering)
                return true;

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


            case static_cast<uint16_t>(Hardware::SwId::ENC_4_SW): // pitch knob
            {
                const int page = knob_page[0];
                pitch_reset = rising;

                if(rising)
                {
                    if(page == 0)
                    {
                        enc_values[0][0] = enc_defaults[0][0];
                        fx_->SetGlobalPitch(1.f);
                        fx_->SetReverse(false);
                        fx_->ResetGlobalPitchQuant();
                    }
                    else if(rising && page == 1)
                    {
                        enc_values[1][0] = enc_defaults[1][0];
                        fx_->SetPan(.5f);
                        fx_->SetGain(.704f);
                    }
                    else
                        arp_knob.Off();

                    DumpValuePresets();
                    leds.ui.Pth(1, 1.f, 1.f, 1.f);
                }
            break;
            }

            case ENC_5_SW:
                if(!rising)
                    fx_->CycleTransportMode();
            break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_1_SW): // attack knob
                if(rising)
                {
                    fx_->ToggleAutoLoop();
                    float on = fx_->GetAutoLoop();
                    DumpValuePresets();
                    leds.ui.Pth(2, on, on, on);
                }
            break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_2_SW): // decay knob
                if(rising)
                {
                    fx_->ToggleSustainActive();
                    float on = fx_->GetSustainActive();
                    DumpValuePresets();
                    leds.ui.Pth(3, on, on, on);
                }
            break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_6_SW): // volume
                if(rising)
                {
                    fx_->IncrementMonitorMode();
                    input_toggled = true;
                }
            break;

            case static_cast<uint16_t>(Hardware::SwId::ENC_3_SW): // magic wand
            {
                fx_reset = rising;
                if(rising)
                    ResetFx();
            }

            case static_cast<uint16_t>(Hardware::SwId::SW_TOG): // toggle (no longer used, here for safety)
                break;

            case static_cast<uint16_t>(Hardware::SwId::KEY_26): // chompi
            {
                chompi_key_pressed = rising;

                if(rising && selected_slot != kSlotNone)
                {
                    if(preset_mode == PresetMode::SAVE_SEL)
                    {
                        fx_->StopAllVoices();

                        FileCopier::CopyRequest::RamDir looper = selected_slot == 16 ? 
                                                                FileCopier::CopyRequest::RamDir::TO
                                                                : FileCopier::CopyRequest::RamDir::NONE;

                        FileCopier::CopyRequest req( 15, cs_bank, cs_mode, 
                            selected_slot, ss_bank, ss_mode, selected_slot != 16, 
                            FileCopier::CopyRequest::RamDir::FROM, looper);

                        copier_->Request(req);

                        blink_startt = System::GetNow();

                        preset_mode = PresetMode::SAVING;
                        presets_->Save(static_cast<uint8_t>(ss_mode), ss_bank, selected_slot);
                    }
                    else if(preset_mode == PresetMode::COPY_DEST)
                    {
                        fx_->StopAllVoices();

                        FileCopier::CopyRequest::RamDir looper = FileCopier::CopyRequest::RamDir::NONE;
                        FileCopier::CopyRequest::RamDir chompi = FileCopier::CopyRequest::RamDir::NONE;

                        if(copy_src == 15)
                            chompi = FileCopier::CopyRequest::RamDir::FROM;
                        else if(copy_src == 16)
                            looper = FileCopier::CopyRequest::RamDir::FROM;

                        if(selected_slot == 15)
                            chompi = FileCopier::CopyRequest::RamDir::TO;
                        else if(selected_slot == 16)
                            looper = FileCopier::CopyRequest::RamDir::TO;

                        bool set_slot = !(selected_slot == 16) && !(selected_slot == 15 && ss_mode == VoiceMode::CUBBI);
                        FileCopier::CopyRequest req( copy_src, cs_bank, cs_mode,
                            selected_slot, ss_bank, ss_mode, set_slot,
                            chompi, looper);
                        req.append = looper_append && !fx_->IsLooperFirstRecording();

                        copier_->Request(req);

                        blink_startt = System::GetNow();

                        preset_mode = PresetMode::COPYING;
                        presets_->Copy(static_cast<uint8_t>(cs_mode), cs_bank, copy_src, static_cast<uint8_t>(ss_mode), ss_bank, selected_slot);
                    }
                    else if(preset_mode == PresetMode::ERASE_SEL)
                    {
                        // if a voice is playing that slot, stop it
                        fx_->StopAllVoices();

                        fx_->EraseStart(selected_slot, ss_bank, ss_mode);
                        preset_mode = PresetMode::ERASING;
            
                        blink_startt = System::GetNow();

                        presets_->Invalidate(static_cast<uint8_t>(ss_mode), ss_bank, selected_slot);
                    }
                }
                return false;
                break;
            }

            case static_cast<uint16_t>(Hardware::SwId::KEY_16): // Jammi mode bank, fall through
            case static_cast<uint16_t>(Hardware::SwId::KEY_17): // Cubbi mode bank
            {                
                if(rising && !no_sd_card_)
                {
                    VoiceMode mode;
                    if(buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_16))
                        mode = VoiceMode::JAMMI;
                    else
                        mode = VoiceMode::CUBBI;

                    if(fx_->GetVoiceMode() == mode)
                    {
                        fx_->IncrementBank();
                        if(mode == VoiceMode::CUBBI)
                            fx_->StopAllVoices();
                    }
                    else
                    {
                        fx_->SetVoiceMode(mode); // change modes
                        if(mode == VoiceMode::JAMMI)
                        {
                            fx_->SetBank(fx_->GetVoiceBank());
                            SetVoiceSlot(fx_->GetVoiceSlot()); // crucially, includes settings
                        }

                        fx_->StopAllVoices();
                    }
                }
                else if(!rising)
                    return false; // note off falls through

            }
            break;

            case static_cast<uint16_t>(Hardware::SwId::KEY_18): // mic in, fall through
            case static_cast<uint16_t>(Hardware::SwId::KEY_19): // aux in, fall through
            case static_cast<uint16_t>(Hardware::SwId::KEY_20): // resample
            {
                if(rising)
                {
                    InputSource source;
                    was_usb = fx_->GetInputSource() == InputSource::USB;
                    if(buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_18))
                        source = InputSource::MIC;
                    else if(buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_19))
                        source = InputSource::LINE_IN;
                    else
                        source = InputSource::RESAMPLE;

                    fx_->PickInputSource(source);
                    if(source == InputSource::LINE_IN || source == InputSource::MIC)
                        StartLongPress(buttonID);
                }
                else
                {
                    if(long_press_key == buttonID)
                        long_press_key = kNoKey;
                    return false; // note off falls through
                }

                break;
            }


            case static_cast<uint16_t>(Hardware::SwId::KEY_21): // fx pre looper, fall through
            case static_cast<uint16_t>(Hardware::SwId::KEY_22): // fx post looper
            {
                if(rising)
                {
                    fx_->SetFxPreLooper(buttonID == static_cast<uint16_t>(Hardware::SwId::KEY_21));
                }
                else if(!rising)
                    return false; // note off falls through

                break;
            }


            case static_cast<uint16_t>(Hardware::SwId::KEY_23): // erase
            {
                if(rising && !no_sd_card_)
                {
                    if(preset_mode == PresetMode::NONE)
                        preset_mode = PresetMode::ERASE_SEL;
                    else if(preset_mode == PresetMode::ERASE_SEL)
                        preset_mode = PresetMode::NONE;
                    else
                        break;

                    selected_slot = kSlotNone;
                    ss_bank = kSlotNone;
                    ss_mode = VoiceMode::LAST;
                }
                else if(!rising)
                    return false; // note off falls through

                break;
            }

            case static_cast<uint16_t>(Hardware::SwId::KEY_24): // copy
            {
                if(rising)
                    StartLongPress(buttonID);
                else if(long_press_key == buttonID)
                    long_press_key = kNoKey;
                if(rising && !no_sd_card_)
                {                    
                    if(preset_mode == PresetMode::NONE)
                        preset_mode = PresetMode::COPY_SRC;
                    else if(preset_mode == PresetMode::COPY_SRC 
                            || preset_mode == PresetMode::COPY_DEST)
                        preset_mode = PresetMode::NONE;
                    else
                        break;

                    copy_src = kSlotNone;
                    selected_slot = kSlotNone;
                    ss_bank = kSlotNone;
                    cs_bank = kSlotNone;
                    ss_mode = VoiceMode::LAST;
                    cs_mode = VoiceMode::LAST;

                }
                else if(!rising)
                    return false; // note off falls through

                break;
            }

            case static_cast<uint16_t>(Hardware::SwId::KEY_25): // save
            {
                if(rising && fx_->GetFileExists(14) && !no_sd_card_)
                {
                    if(preset_mode == PresetMode::NONE)
                        preset_mode = PresetMode::SAVE_SEL;
                    else if(preset_mode == PresetMode::SAVE_SEL)
                        preset_mode = PresetMode::NONE;
                    else
                        break;

                    copy_src = kSlotNone;
                    selected_slot = kSlotNone;
                    ss_bank = kSlotNone;
                    cs_bank = kSlotNone;
                    ss_mode = VoiceMode::LAST;
                    cs_mode = VoiceMode::LAST;
                }                
                else if(!rising)
                    return false; // note off falls through

                break;
            }

            // white keys and play/pause
            default:
                if(buttonID == 33 || buttonID == 34) // both together: loop edit mode on or off
                {
                    const int other = buttonID == 33 ? 34 : 33;
                    if(rising && hw_->button_sr.State(other))
                    {
                        fx_->SetLoopEdit(!fx_->LoopEdit());
                        long_press_key = kNoKey;
                    }
                    else if(preset_mode == PresetMode::NONE)
                    {
                        if(rising)
                            StartLongPress(buttonID);
                        else if(long_press_key == buttonID)
                        {
                            long_press_key = kNoKey;
                            fx_->IncrementLooperDubGain(buttonID == 33 ? -.1f : .1f);
                        }
                    }
                }

                if (rising)
                {
                    size_t slot_req = KeyToSlot(buttonID);
                    if(buttonID == 33 || buttonID == 34)
                        slot_req = 16;

                    if(slot_req == kSlotNone)
                    {
                        // do nothing
                    }
                    else if(slot_req == 16)
                    {
                        if (preset_mode == PresetMode::COPY_DEST && copy_src != 16)
                        {
                            looper_append = buttonID == 34;
                            selected_slot = slot_req;
                            ss_bank = fx_->GetBank();
                            ss_mode = fx_->GetVoiceMode();
                        }
                        else if (preset_mode == PresetMode::COPY_SRC && !fx_->GetLooperIsEmpty())
                        {
                            copy_src = slot_req;
                            cs_bank = fx_->GetBank();
                            cs_mode = fx_->GetVoiceMode();
                            preset_mode = PresetMode::COPY_DEST;
                        }
                    }
                    else if(
                        preset_mode == PresetMode::COPY_SRC
                        && fx_->GetFileExists(slot_req - 1)
                    )
                    {
                        copy_src = slot_req;
                        cs_bank = fx_->GetBank();
                        cs_mode = fx_->GetVoiceMode();

                        preset_mode = PresetMode::COPY_DEST;
                    }
                    else if(preset_mode == PresetMode::COPY_DEST 
                            && (copy_src != slot_req || fx_->GetBank() != cs_bank || fx_->GetVoiceMode() != cs_mode)
                            )
                    {
                        ss_bank = fx_->GetBank();
                        ss_mode = fx_->GetVoiceMode();
                        selected_slot = slot_req;
                    }
                    else if(preset_mode == PresetMode::NONE 
                            && fx_->GetFileExists(slot_req - 1)
                            && fx_->GetVoiceMode() == VoiceMode::JAMMI
                            && !no_sd_card_)
                    {
                        ss_bank = fx_->GetBank();
                        ss_mode = fx_->GetVoiceMode();
                        selected_slot = slot_req;
                        SetVoiceSlot(selected_slot);
                    }
                    else if(
                        preset_mode == PresetMode::ERASE_SEL 
                        && slot_req != 15
                        && fx_->GetFileExists(slot_req - 1)
                    )
                    {
                        ss_bank = fx_->GetBank();
                        ss_mode = fx_->GetVoiceMode();
                        selected_slot = slot_req;
                    }
                    else if(preset_mode == PresetMode::SAVE_SEL && slot_req != 15)
                    {
                        ss_bank = fx_->GetBank();
                        ss_mode = fx_->GetVoiceMode();
                        selected_slot = slot_req;
                    }
                }
                else if(buttonID < 29) // white keys, no play / pause
                {
                    return false; // allow releasing notes in shift menu
                }
                break;
            }

            return true;
        }

        void ResetFx()
        {
            enc_values[0][3] = enc_defaults[0][3];
            enc_values[1][3] = enc_defaults[1][3];
            enc_values[2][3] = enc_defaults[2][3];

            fx_->SetReverbGrains(enc_values[0][3], 0.f);
            fx_->SetDelayFeedback(enc_values[0][3]);
            grains.Thaw();
            fx_->SetSaturate(enc_values[1][3]);
            fx_->SetFilter(enc_values[2][3]);

            resonance = 0.f;
            warble = 0.f;

            fx_->SetFilterResonance(resonance);
            fx_->SetDelayTime(.5f);
            fx_->SetWarble(warble);

            if (split_delay_) {
                enc_values[0][3] = .5f;
            }
        }

        void OnFocusGained() override
        {
            fx_reset = false;
            pitch_reset = false;
            chompi_key_pressed = true;
            long_press_key = kNoKey; // its release may have gone to the page below

            if(quantized_pitch_)
            {
                fx_->ResetGlobalPitchQuant();
                fx_->ResetLooperPitchQuant();
            }

            input_toggled = false;
            wheel_turned = false;

            ss_bank = fx_->GetBank();
            copy_src = kSlotNone;
            selected_slot = fx_->GetVoiceSlot();
            preset_mode = PresetMode::NONE;
        }

        inline void SetSwitchState(bool state) { switch_state = state; }
        void SetOptions(bool ps_quant, bool split_delay)
        {
            quantized_pitch_ = ps_quant;
            split_delay_ = split_delay;
        }

        bool IsClosable()
        { 
            if(
                System::GetNow() - blink_startt > 1000                              // animation done AND
                && ((preset_mode == PresetMode::SAVING && !copier_->IsCopying())    // (finished saving OR
                || (preset_mode == PresetMode::COPYING && !copier_->IsCopying())    // finished copying OR
                || (preset_mode == PresetMode::ERASING && !fx_->IsErasing())        // finished erasing OR
                || (preset_mode == PresetMode::NONE && !chompi_key_pressed)         // did nothing OR
                || (preset_mode != PresetMode::COPYING  && preset_mode != PresetMode::SAVING 
                    && preset_mode != PresetMode::ERASING && !switch_state)) // NOT working on a copy/save/erase and the mode switch went up)
            )
            {
                if((preset_mode == PresetMode::SAVING || preset_mode == PresetMode::COPYING) && selected_slot == 16)
                {
                    // a staged load switches over by itself
                    if(!copier_->IsLooperStaged())
                    {
                        fx_->LooperOpenFile(copier_->LooperWasPlaying());
                        enc_values[0][4] = enc_defaults[0][4];
                    }
                }
                else if((preset_mode == PresetMode::COPYING || preset_mode == PresetMode::SAVING) && ss_mode != VoiceMode::CUBBI)
                {
                    SetVoiceSlot(selected_slot);
                }
                else if(preset_mode == PresetMode::ERASING
                    && fx_->GetVoiceMode() == VoiceMode::JAMMI
                    && fx_->GetVoiceSlot() == selected_slot)
                {
                    SetVoiceSlot(15);
                }

                if(!quantized_pitch_)
                {
                    fx_->ResetGlobalPitchQuant();
                    fx_->ResetLooperPitchQuant();
                }

                return true;
            }

            return false;
        }

        bool no_sd_card_ = false;
        inline void NoSDCard(bool gone) { no_sd_card_ = gone; }
        bool card_mode_ = false, card_mode_request_ = false; // the card as a USB drive
        void CardMode(bool on) { card_mode_ = on; }
        bool TakeCardModeRequest()
        {
            const bool request = card_mode_request_;
            card_mode_request_ = false;
            return request;
        }

    private:
        Hardware *hw_;
        Engine *fx_;
        FileCopier *copier_;
        PresetManager* presets_;
        float** enc_values;
        const float** enc_defaults;
        uint8_t* knob_page;

        bool quantized_pitch_ = true;
        bool split_delay_ = false;

        const float* key_color;

        bool input_toggled;
        bool wheel_turned = false; // since SHIFT went down

        bool chompi_key_pressed = false;
        bool blink_state = true;
        uint32_t blink_startt;
        uint32_t last_blink;

        float resonance, warble;
        float final_comp;

        bool fx_reset = false;
        bool pitch_reset = false;

        uint8_t selected_slot = kSlotNone;
        uint8_t ss_bank = kSlotNone;
        VoiceMode ss_mode = VoiceMode::LAST;
        uint8_t copy_src = kSlotNone;
        bool looper_append = false;
        static const uint16_t kNoKey = 0xffff;
        static const uint32_t kLongPress = 500; // ms
        uint16_t long_press_key = kNoKey;
        bool was_usb = false; // before the key went down
        uint32_t long_press_down = 0;
        uint32_t bake_refused = 0;
        uint32_t undo_flash = 0;
        bool undo_ok = false;

        void StartLongPress(uint16_t key)
        {
            long_press_key = key;
            long_press_down = System::GetNow();
        }
        uint8_t cs_bank = kSlotNone;
        VoiceMode cs_mode = VoiceMode::LAST;
        bool switch_state;

        PresetMode preset_mode = PresetMode::NONE;
    };
} // namespace chompi

#pragma GCC pop_options
