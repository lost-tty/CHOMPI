#include "hardware.h"
#include "temp_led_stuff.h"
#include "ui.h"
#include "daisysp.h"
#include "fatfs.h"
#include "diskio.h"
#include "DSPEngine.h"
#include "FileCopier.h"
#include "RamBuffer.h"
#include "InterpolatedDelayLine.h"
#include "OptionsManager.h"
#include "Clock.h"
#include <cerrno>

#define DSY_DTCMRAM_BSS __attribute__((section(".dtcmram_bss")))

using namespace daisy;
using namespace chompi;

Hardware hw;
UserInterface ui;

SdmmcHandler sdmmc;
FatFSInterface fsi;
Engine engine;
PresetManager presets;
OptionsManager options;

daisysp::Reverb DSY_DTCMRAM_BSS reverb;
Clock DSY_DTCMRAM_BSS chompi::beat_clock;
LoopSync DSY_DTCMRAM_BSS daisy::loop_sync;
AiffInfo DSY_DTCMRAM_BSS chompi::slot_aiff;
uint16_t DSY_DTCMRAM_BSS chompi::kit_slots[5];
RamBufferMemory DSY_DTCMRAM_BSS chompi::kit_buff;
KitLoader DSY_DTCMRAM_BSS chompi::kit_loader;
int16_t DSY_SDRAM_BSS kit_mem[kKitMaxFrames * 2];
FIL DSY_SDRAM_BSS kit_fil; // the SD DMA can't reach DTCM
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) kit_bounce[kKitBounce];
chompi::InterpolatedDelayLine::AudioSample DSY_SDRAM_BSS del_mem[kMaxDelayTime];

RamBufferMemory loop_buff;
int16_t DSY_SDRAM_BSS loop_mem[kMaxRamBuffSize]; 
UndoMemory DSY_DTCMRAM_BSS loop_undo;

RamBufferMemory chompi_buff;
// the pre-roll lives behind the buffer, 8K keeps the next one aligned
int16_t DSY_SDRAM_BSS chompi_mem[kMaxRamBuffSize + 4096]; 

daisysp::Oscillator osc;

// CpuLoadMeter meter;
uint32_t pret, sd_checkt;
// bool log_batt;
bool booting = true;
bool rainbow_done = false;

FileCopier copier;

/** breakdown:
 *  Inputs:
 *  Channel 1 - Microphone
 *  Channel 2 - X
 *  Channel 3 - Aux L
 *  Channel 3 - Aux R
 *
 *  Outputs:
 *  Channel 1 - Headphone L
 *  Channel 2 - Headphone R
 *  Channel 3 - Master L
 *  Channel 4 - Master R
 */
bool line_in_state;
void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    // meter.OnBlockStart();

    if(booting && !ui.InTestMode())
    {
        hw.ProcessAllControls();
        ui.GenerateEvents();
        ui.DoEvents();

        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;
        }

        return;
    }

    hw.ProcessAllControls();
    ui.GenerateEvents();
    engine.Prepare();

    // does this have to happen in the audio callback?
    if(hw.jack_detect.Read() != line_in_state)
    {
        if(hw.jack_detect.Read())
        {
            engine.SetInputSource(InputSource::LINE_IN);
            engine.ResetLineMono();
        }
        else
            engine.SetInputSource(InputSource::MIC);
    }
    line_in_state = hw.jack_detect.Read();

    if(ui.InTestMode() && ui.GetToggleState())
    {
        for(size_t i = 0; i < size; i++)
        {
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = osc.Process();
        }
    }
    else
        engine.Process(in, out, size);

    // meter.OnBlockEnd();
}

// The heap. The SRAM is full, so newlib's would start in the last few bytes of it and the first malloc
// (USB on plugging into a computer, atof's bignums) would write past the end of RAM: a bus fault.
// It lives in DTCM instead, and running out gives NULL. The CPU is all that touches it, so DTCM is fine.
static constexpr size_t kHeapSize = 8 * 1024;
uint8_t DSY_DTCMRAM_BSS heap_mem[kHeapSize];
size_t heap_used; // .bss, zeroed before anything can malloc

extern "C" void* _sbrk(ptrdiff_t incr)
{
    if(incr < 0 || heap_used + incr > kHeapSize)
    {
        errno = ENOMEM;
        return reinterpret_cast<void*>(-1);
    }
    void* prev = &heap_mem[heap_used];
    heap_used += incr;
    return prev;
}

void ZeroSDRAM()
{
    uint32_t *beg, *end;
    size_t    size_in_words = (1024 * 1024 * 64) / sizeof(uint32_t);
    beg                     = (uint32_t*)0xc0000000;
    end                     = (uint32_t*)(beg + size_in_words);
    std::fill(beg, end, 0);
}

bool no_sd_card = false;
void CheckSDCardMounted()
{
    DSTATUS res = disk_status(0);

    // lost the SD card, must reboot
    if(res != RES_OK)
    {
        no_sd_card = true;
        ui.NoSDCard();
        
        engine.SetVoiceMode(VoiceMode::JAMMI);
        engine.SetVoiceSlot(15, true);

        const uint32_t start_time = System::GetNow();
        while(System::GetNow() - start_time < 3000)
        {
            ui.NoSDCardAnimation(true);
            ui.DoEvents();
            System::Delay(1);
        }

        ui.NoSDCardAnimation(false);
    }
}

void SDCallback(void* data)
{
    const uint32_t now = System::GetNow();
    if (now - sd_checkt > 1000 && !no_sd_card && !booting)
    {
        sd_checkt = now;
        CheckSDCardMounted();
    }
    else if(no_sd_card)
        return;

    if(copier.CopyProcess())
    {
        ui.DoEvents();
        return;
    }

    kit_loader.Step();
    engine.ProcessFileRequests();

    if(now - pret > 50 && !engine.AnyVoicesPlaying())
    {
        pret = now;
        ui.WritePresets();
        kit_loader.Save(now);
    }    
}

uint32_t uit, now, pre_startt;

#if !NO_BATT
uint32_t batt, usbt;
bool prev_int_state = true;
bool usb_handoff = false;
#endif

uint8_t preset = 0;
uint8_t bank = 0; 
uint8_t mode = 0;


void MainLoop(void* data)
{
    if(booting)
    {
        hw.LowBatteryLockoutCheck();

        if(!copier.IsCopying())
        {
            if(copier.NeedsOverwrite(preset, VoiceMode(mode), bank))
            {
                FileCopier::CopyRequest req(preset + 1, bank, VoiceMode(mode),
                    preset + 1,bank, VoiceMode(mode), false,
                    FileCopier::CopyRequest::RamDir::NONE,
                    FileCopier::CopyRequest::RamDir::NONE);

                copier.req_fifo.PushBack(req);

                System::Delay(5); // fixes data race
            }

            preset++;
            if(preset >= 14)
            {
                preset = 0;
                bank++;

                if(bank >= 5)
                {
                    bank = 0;
                    mode++;

                    if(mode >= 2)
                    {
                        engine.UpdateFileExists();
                        booting = false;
                    }
                }
            }
        }

        return;
    }
    else if(!rainbow_done)
    {
        ui.StopBootAnimation();
        ui.RainbowWave();
        rainbow_done = true;
    }

    // volatile float avg_load = meter.GetAvgCpuLoad();
    // volatile float max_load = meter.GetMaxCpuLoad();
    now = daisy::System::GetNow();

    if (now - uit > 1)
    {
        ui.DoEvents();
        ui.ProcessMidi();
        uit = now;
    }

    if (now - pre_startt > 5000)
    {
        ui.TestPresets();
        pre_startt = now;
    }

    // update now to actually be now
    now = daisy::System::GetNow();

    #if !NO_BATT
    // falling edge battery interrupt
    bool int_state = hw.mpc_int.Read();
    bool interrupt = !int_state && prev_int_state;
    prev_int_state = int_state;

    if(ui.InRainbows())
    {
        batt = now;
    }
    else if(now - batt > 20)
    {
        hw.LowBatteryLockoutCheck();
        batt = now;
    }

    if(ui.InTestMode())
    {
        hw.MpReadAll();

        while (!hw.read_ready) {
            System::Delay(1);
        }
        ui.TestPowerCable(hw.mp_buff_[1] >> 5 & 1); //VIN_RDY

        // Normal NTC_MISSING, BATT_MISSING, NTC1_FAULT, and NTC2_FAULT
        ui.TestBMC(hw.mp_buff_[3] == 0); 
    }


    if(interrupt)
    {
        // last_read = now;
        hw.MpReadAll();

        while (!hw.read_ready) {
            System::Delay(1);
        }
        // Unknown or (not started and interrupt)
        uint8_t masked = hw.mp_buff_[0] & 0B11110000; // DPDM_STAT
        if( masked == 0 )
        {
            hw.USBMidiActive(false);
            hw.usb_sw.Write(false); // give USB control
            usb_handoff = true;
            usbt = now;
        }
        else
        {
            hw.usb_sw.Write(true); // take USB control            
            hw.USBMidiActive(true);
        }
    }

    if(usb_handoff && now - usbt > 1)
    {
        usb_handoff = false;
        hw.MpWrite(0x0a, 0B00110100); // FORCEDPDM
    }
    #endif

    System::DelayUs(10);
}

void daisy::MidiRxByte(uint8_t byte, const void* source) { beat_clock.Byte(byte, source); }

int main(void)
{
    beat_clock.Init(); // DTCM isn't zeroed, and hw.Init starts MIDI
    loop_sync.Init();
    slot_aiff = AiffInfo();
    std::fill(kit_slots, kit_slots + 5, 0);
    kit_buff = RamBufferMemory();
    kit_buff.Init(kit_mem);
    kit_loader.Init(&kit_fil, kit_bounce);
    hw.Init();
    // System::Delay(100);

    hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on 

    hw.MpReadAll();

    for(size_t i = 0; i < 10; i++)
    {
        hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    /** SDMMC Init */
    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    // sd_cfg.clock_powersave = true;
    sdmmc.Init(sd_cfg);
    fsi.Init(FatFSInterface::Config::MEDIA_SD);
    f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1);
    // our own folder on a multi-firmware card, else the one shared with TAPE, else the root
    if(f_chdir("/MIXTAPE") != FR_OK)
        f_chdir("/TAPE");

    // delete the battery log if it exists
    char filename[32];
    sprintf(filename, ".batt_log.txt");
    f_unlink(filename);

    // macos makes a copy
    sprintf(filename, "._.batt_log.txt");
    f_unlink(filename);


    options.Init();

    LedSetup();
    ui.Init(&hw, &engine, &copier, &presets,
        options.midi_ch_in, options.midi_ch_out, options.pitch_shift_quantization, options.delay_split);

    hw.StartLowPriorityCallback(SDCallback, 1000);
    hw.StartAudio(AudioCallback);

    ZeroSDRAM();

    // meter.Init(hw.seed.AudioSampleRate(), hw.seed.AudioBlockSize());

    loop_buff.Init(&loop_mem[0], &loop_undo);
    chompi_buff.Init(&chompi_mem[0]);
    engine.Init(hw.seed.AudioSampleRate(), &reverb, &del_mem[0], 
                &loop_buff, &chompi_buff, 
                options.record_latch, options.tape_slew_on,
                MonitorMode(options.monitor_position));

    osc.Init(hw.seed.AudioSampleRate());
    osc.SetAmp(.2f);

    now = daisy::System::GetNow();
    uit = now;
    pret = now;
    pre_startt = now;

    #if !NO_BATT
    usbt = now;
    batt = now;
    #endif

    // get any junk out of the SRs, takes .5s
    uint32_t vol_state = 0;
    uint32_t sleep_state = 0;

    for(int i = 0; i < 5000; i++)
    {
        hw.ProcessAllControls();
        vol_state += hw.button_sr.State(int(Hardware::SwId::ENC_6_SW));
        sleep_state += hw.button_sr.State(int(Hardware::SwId::KEY_26))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_27))
                        && hw.button_sr.State(int(Hardware::SwId::KEY_28));

        System::DelayUs(100);
    }

    if(sleep_state > 4000)
        hw.MpWrite(0x08, 0B10111111); // SHIPPING MODE
    else if(vol_state > 4000)
        ui.TestMode();

    hw.usb_sw.Write(false);     // give USB control
    daisy::System::Delay(1); // Wait a sec
    hw.MpWrite(0x0a, 0B00100100); // Auto DPDM
    daisy::System::Delay(1); // Wait a sec
    hw.usb_sw.Write(true);     // take USB control
    
    copier.Init(hw.seed.AudioSampleRate(), &engine, &chompi_buff, &loop_buff);
    engine.FillDefaultSample(hw.seed.AudioSampleRate());

    while (1)
    {
        MainLoop(nullptr);
    }
}