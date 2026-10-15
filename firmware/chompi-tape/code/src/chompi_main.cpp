#include "hardware.h"
#include "Leds.h"
#include "ui.h"
#include "daisysp.h"
#include "fatfs.h"
#include "diskio.h"
#include "ff_gen_drv.h"
extern "C" Disk_drvTypeDef disk; // FatFS's driver table, defined in ff_gen_drv.c
#include "DSPEngine.h"
#include "FileCopier.h"
#include "Streamer.h"
#include "RamBuffer.h"
#include "InterpolatedDelayLine.h"
#include "OptionsManager.h"
#include "Clock.h"

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
Arp DSY_DTCMRAM_BSS chompi::arp;
ArpKnob DSY_DTCMRAM_BSS chompi::arp_knob;
AiffInfo DSY_DTCMRAM_BSS chompi::slot_info;
uint16_t DSY_DTCMRAM_BSS chompi::kit_slots[5];
RamBufferMemory DSY_DTCMRAM_BSS chompi::slot_buff;
SlotLoader DSY_DTCMRAM_BSS chompi::slot_loader;
SdDma DSY_DTCMRAM_BSS chompi::sd_dma;
Streamer DSY_DTCMRAM_BSS chompi::streamer;
Stream DSY_DTCMRAM_BSS daisy::voice_streams[kStreams];
SdFile DSY_SDRAM_BSS slot_file, copy_files[2]; // the SD DMA can't reach DTCM
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) slot_bounce[kSlotBounce];
SdFile DSY_SDRAM_BSS stream_files[kStreams];
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) stream_head[512];
static constexpr int kStreamBlocks = 16; // 1 MB
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) stream_cache[kStreamBlocks * Streamer::kBlockBytes];
int16_t DSY_SDRAM_BSS stream_rings[kStreams * 2 * Ring::kFrames * 2];
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) copy_in[SdDma::kBuffer];
uint8_t DSY_SDRAM_BSS __attribute__((aligned(32))) copy_out[SdDma::kBuffer];
chompi::InterpolatedDelayLine::AudioSample DSY_SDRAM_BSS del_mem[kMaxDelayTime];
Grains DSY_DTCMRAM_BSS chompi::grains;
Grains::Frame DSY_SDRAM_BSS grain_mem[Grains::kFrames];

// the heap ends at _eheap: the default _sbrk runs past the end of RAM
extern "C" uint8_t _sheap[], _eheap[];
extern "C" void *_sbrk(ptrdiff_t incr)
{
    static size_t used; // .bss is zeroed, D3 isn't
    if(incr > _eheap - _sheap - ptrdiff_t(used) || ptrdiff_t(used) + incr < 0)
    {
        errno = ENOMEM;
        return reinterpret_cast<void *>(-1);
    }
    void *p = _sheap + used;
    used += incr;
    return p;
}

RamBufferMemory loop_buff;
int16_t DSY_SDRAM_BSS loop_mem[kMaxRamBuffSize]; 
UndoMemory DSY_DTCMRAM_BSS loop_undo;

RamBufferMemory chompi_buff;
// the pre-roll lives behind the buffer, 8K keeps the next one aligned
int16_t DSY_SDRAM_BSS chompi_mem[kMaxRamBuffSize + kRecRing];

daisysp::Oscillator osc;

// CpuLoadMeter meter;
uint32_t pret, sd_checkt;
// bool log_batt;
bool sound_up = false;  // the engine plays: the keys are live, on the SD timer's ticks until init_done
bool init_done = false; // main runs the UI and the SD timer the card

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

static_assert(Hardware::kAudioBlock <= usb::kMaxBlock && Hardware::kAudioBlock <= Engine::kMaxBlock,
              "USB and the engine hold one audio block");

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
    // meter.OnBlockStart();

    if(!sound_up)
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = 0.f;
        return;
    }

    hw.ProcessAllControls();
    ui.GenerateEvents();
    engine.Prepare();

    // a recording held from this press starts at it
    static bool chompi_key = false;
    if(hw.ChompiKeyDown() && !chompi_key)
        engine.MarkRecordPress(hw.KeyLag() * size);
    chompi_key = hw.ChompiKeyDown();

    // does this have to happen in the audio callback?
    if(hw.jack_detect.Read() != line_in_state)
    {
        if(hw.jack_detect.Read())
        {
            engine.PickInputSource(InputSource::LINE_IN);
            engine.ResetLineMono();
        }
        else
            engine.PickInputSource(InputSource::MIC);
    }
    line_in_state = hw.jack_detect.Read();

    engine.UpdateUsbInputSource(usb::Connected(), usb::RxStreaming(), line_in_state);
    engine.SetUsbInput(usb::Receive(size));

    // to the host: the mix before the volume, the input as recorded
    const float *to_host[usb::kTxChannels] = {out[2], out[3], nullptr, nullptr};
    if(ui.InTestMode() && ui.GetToggleState())
    {
        for(size_t i = 0; i < size; i++)
            out[0][i] = out[1][i] = out[2][i] = out[3][i] = osc.Process();
    }
    else
    {
        engine.Process(in, out, size);
        to_host[0] = engine.MainMix()[0];
        to_host[1] = engine.MainMix()[1];
        to_host[2] = engine.RecordInput()[0];
        to_host[3] = engine.RecordInput()[1];
    }
    usb::Send(to_host, size);

    // meter.OnBlockEnd();
}

// Lets go of the card; nothing touches it until it is mounted again.
void DropCard()
{
    copier.Detach();
    ui.DropPresetsWrite();
    f_mount(nullptr, fsi.GetSDPath(), 0);
}

// When the card is pulled while running, the RAM sample and the loop play on and nothing loads or saves.
// The SD timer sees it go and answer again; main takes it back and reads it like at boot.
volatile bool card_gone = false, card_answers = false;
bool card_shown = false; // what the UI was last told
void CardLost()
{
    card_gone = true;
    card_answers = false;
    engine.StopStreams();
    slot_loader.Detach();
    DropCard();
}

// libDaisy's SD interrupt, wrapped by the linker (Makefile): a transfer of the SD DMA that ends runs the SD timer at once
extern "C" void __real_HAL_SD_IRQHandler(SD_HandleTypeDef *hsd);
extern "C" void __wrap_HAL_SD_IRQHandler(SD_HandleTypeDef *hsd)
{
    __real_HAL_SD_IRQHandler(hsd);
    if(sd_dma.Active() && hsd->State == HAL_SD_STATE_READY)
        TIM4->EGR = TIM_EGR_UG;
}

// our own folder on a multi-firmware card, else the one shared with TAPE, else the root
bool MountCard()
{
    if(f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1) != FR_OK)
        return false;
    if(f_chdir("/MIXTAPE") != FR_OK)
        f_chdir("/TAPE");
    return true;
}

// Reads the card: the engine takes its options now, the UI takes the options and the presets on its tick.
void ReadCard()
{
    options.Init();
    engine.SetTapeSlew(options.tape_slew_on);
    engine.SetMonitorMode(MonitorMode(options.monitor_position));
    ui.ReadCard();
    engine.UpdateFileExists();
    copier.StartScan();
}

// Mounts and reads the card anew from main while the SD timer leaves it alone; the UI takes it at once.
bool TakeCard()
{
    if(!MountCard())
        return false;
    streamer.Forget();
    ReadCard();
    ui.ApplyCard();
    return true;
}

void CardBack()
{
    if(TakeCard())
    {
        engine.ReopenStreams();
        card_gone = false;
    }
    card_answers = false;
}

// the card as a USB drive: CHOMPI lets go of it while the host has it, and takes it back as it finds it.
// All card work runs on the SD timer; while card_lent it serves only the host, and this runs alone on the card
bool card_lent = false;
void CardMode(bool on)
{
    if(on)
    {
        card_lent = true;
        while(sd_dma.Active()) // the SD timer ends the transfer under way
            System::Delay(1);
        engine.StopAllVoices();
        slot_loader.Release();
        DropCard();
        usb::CardMode(true);
    }
    else
    {
        usb::CardMode(false);
        while(sd_dma.Active())
            System::Delay(1);
        const bool back = TakeCard();
        card_gone = !back;
        card_lent = false;
        if(back) // the host may have changed the slot's file
            engine.SetVoiceSlot(engine.GetVoiceSlot(), true);
    }
    ui.CardMode(on);
}

void SDCallback(void* data)
{
    if(!init_done) // the card is main's
    {
        ui.BootTick(System::GetNow(), sound_up);
        return;
    }
    if(sd_dma.Busy()) // a transfer owns the card
        return;
    if(card_lent)
    {
        usb::CardTask();
        return;
    }

    const uint32_t now = System::GetNow();
    if(card_gone) // until main has it back: a card put in answers once it is initialised anew
    {
        if(!card_answers && now - sd_checkt > 1000)
        {
            sd_checkt = now;
            HAL_SD_DeInit(&hsd1);
            disk.is_initialized[0] = 0; // FatFS calls the driver's init only once a run otherwise
            card_answers = disk_initialize(0) == 0;
        }
        return;
    }
    if(now - sd_checkt > 1000)
    {
        sd_checkt = now;
        if(disk_status(0) != 0)
        {
            CardLost();
            return;
        }
    }

    if(copier.CopyProcess()) // a copy or the scan's header read owns the card
        return;

    engine.ProcessFileRequests();

    if(now - pret > 50 && !engine.AnyVoicesPlaying())
    {
        pret = now;
        ui.WritePresets();
        slot_loader.Save(now);
    }

    if(streamer.Step()) // the voices first
        return;
    if(engine.GetFileManager()->request_fifo.IsEmpty())
        slot_loader.Step(); // last, it may start a transfer
}

uint32_t uit, now, pre_startt;

#if !NO_BATT
uint32_t batt, usbt;
bool prev_int_state = true;
bool usb_handoff = false;
#endif

// Watches the keys held from the start: CHOMPI, PLAY and LOOP for shipping mode, the volume click for test
// mode. The device runs meanwhile.
static constexpr uint32_t kBootHold = 400; // ms
uint32_t boot_at = 0;
bool sleep_held = true, vol_held = true, boot_hold_done = false;

void BootHoldStep(uint32_t now)
{
    if(boot_hold_done)
        return;
    sleep_held = sleep_held && hw.button_sr.State(int(Hardware::SwId::KEY_26))
                 && hw.button_sr.State(int(Hardware::SwId::KEY_27)) && hw.button_sr.State(int(Hardware::SwId::KEY_28));
    vol_held = vol_held && hw.button_sr.State(int(Hardware::SwId::ENC_6_SW));
    if(!sleep_held && !vol_held)
        boot_hold_done = true;
    else if(now - boot_at >= kBootHold)
    {
        boot_hold_done = true;
        if(sleep_held)
            hw.MpWrite(0x08, 0B10111111); // SHIPPING MODE
        else
            ui.TestMode();
    }
}

void MainLoop(void* data)
{
    chompi::usb::MainAlive(); // the USB task logs where the CPU is when this stops
    // volatile float avg_load = meter.GetAvgCpuLoad();
    // volatile float max_load = meter.GetMaxCpuLoad();
    now = daisy::System::GetNow();
    BootHoldStep(now);

    if (now - uit > 1)
    {
        ui.DoEvents();
        ui.ProcessMidi();
        uit = now;
        if(card_answers)
            CardBack();
        if(card_gone != card_shown)
        {
            card_shown = card_gone;
            ui.CardGone(card_shown);
        }

        if(ui.TakeCardModeRequest() && (usb::InCardMode() || (usb::Connected() && !engine.Recording() && !copier.IsCopying())))
            CardMode(!usb::InCardMode());
        else if(usb::InCardMode() && (usb::TakeCardEjected() || !usb::Connected()))
            CardMode(false);
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

// MIDI comes in below the audio callback's priority
void daisy::MidiRxByte(uint8_t byte, const void* source)
{
    ScopedIrqBlocker block;
    beat_clock.Byte(byte, source);
}

int main(void)
{
    beat_clock.Init(); // DTCM isn't zeroed, and hw.Init starts MIDI
    loop_sync.Init();
    arp.Init();
    arp_knob.Init();
    slot_info = AiffInfo();
    std::fill(kit_slots, kit_slots + 5, 0);
    slot_buff = RamBufferMemory();
    sd_dma.Init();
    slot_loader.Init(&slot_file, slot_bounce, chompi_mem, kMaxRamBuffSize, &chompi_buff);
    streamer.Init(voice_streams, kStreams, stream_files, stream_head, stream_rings, stream_cache, kStreamBlocks);
    hw.Init();
    // the keys read pressed and the encoders 0xff until scanned through the debounce: before anyone looks
    for(size_t i = 0; i < hw.KeyLag(); i++)
    {
        hw.ProcessAllControls();
        System::Delay(1);
    }
    // The SDRAM is up. The file objects in it start closed; nothing else in it is read before it is written.
    slot_file = copy_files[0] = copy_files[1] = SdFile();
    for(SdFile &f : stream_files)
        f = SdFile();
    leds.Init();
    hw.StartLowPriorityCallback(SDCallback, 1000); // draws the rainbow through init, then it is the card's timer
    // System::Delay(100);

    hw.MpWrite(0x0c, 0B01010001); // set BATT_LOW to 3V, turn on 

    constexpr int kReadWait = 5; // ms a reading takes at most
    for(size_t i = 0; i < 10; i++) // each read lands before the next
    {
        hw.LowBatteryLockoutCheck();
        for(int w = 0; w < kReadWait && !hw.read_ready; w++)
            System::Delay(1);
    }

    loop_buff.Init(&loop_mem[0], &loop_undo);
    chompi_buff.Init(&chompi_mem[0]);
    grains.Init(grain_mem);
    engine.Init(hw.seed.AudioSampleRate(), &reverb, &del_mem[0], &loop_buff, &chompi_buff);
    osc.Init(hw.seed.AudioSampleRate());
    osc.SetAmp(.2f);
    copier.Init(hw.seed.AudioSampleRate(), &engine, &chompi_buff, &loop_buff,
                {&copy_files[0], &copy_files[1], copy_in, copy_out});
    engine.FillDefaultSample(hw.seed.AudioSampleRate());
    ui.Init(&hw, &engine, &copier, &presets, &options);
    hw.StartAudio(AudioCallback);
    sound_up = true;

    hw.usb_sw.Write(false);     // give USB control
    daisy::System::Delay(1);
    hw.MpWrite(0x0a, 0B00100100); // Auto DPDM
    daisy::System::Delay(1);
    hw.usb_sw.Write(true);     // take USB control

    // The card comes up with the device already playing; main's first tick applies what it holds.
    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);
    fsi.Init(FatFSInterface::Config::MEDIA_SD);
    if(MountCard())
    {
        f_unlink(".batt_log.txt");
        f_unlink("._.batt_log.txt"); // macos makes a copy
        ReadCard();
    }
    else
        card_gone = true; // plays the built-in sample until a card is in

    now = daisy::System::GetNow();
    uit = now;
    pret = now;
    pre_startt = now;
    boot_at = now;
    #if !NO_BATT
    usbt = now;
    batt = now;
    #endif
    init_done = true;

    while (1)
    {
        MainLoop(nullptr);
    }
}