#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once
#include <algorithm>
#include "daisy.h"
#include "fatfs.h"
#include "FileStreamingManager.h"
#include "SdDma.h"
#include "Wav.h"

namespace daisy {

// Copies between card slots, the recording and the looper, a chunk per step from the card task, the card by DMA.
// A file is written as a .tmp and renamed over the target when complete.
class FileCopier
{
    public:
        // in SDRAM, 32-byte aligned buffers of SdDma::kBuffer bytes
        struct Memory
        {
            chompi::SdFile *src, *dst;
            uint8_t *in, *out;
        };

        FileCopier() {}
        ~FileCopier() {}

        void Init(float samplerate, Engine* fx, RamBufferMemory* chompi_buff, RamBufferMemory* looper_buff,
                  const Memory &mem)
        {
            fx_ = fx;
            mem_ = mem;
            copying_ = false;

            // WAV file header template
            file_header.ChunkId       = kWavFileChunkId;     /** "RIFF" */
            file_header.FileFormat    = kWavFileWaveId;      /** "WAVE" */
            file_header.SubChunk1ID   = kWavFileSubChunk1Id; /** "fmt " */
            file_header.SubChunk1Size = 16;                  // for PCM
            file_header.AudioFormat   = WAVE_FORMAT_PCM;
            file_header.NbrChannels   = 2;
            file_header.SampleRate    = static_cast<int>(samplerate);
            file_header.ByteRate      = samplerate * 2 * 16 / 8; // sr * chan * bitspersample / 8
            file_header.BlockAlign    = 2 * 16 / 8; //channels * bitspersample / 8;
            file_header.BitPerSample  = 16;
            file_header.SubChunk2ID   = kWavFileSubChunk2Id; /** "data" */

            chompi_ram.Init(chompi_buff);
            looper_ram.Init(looper_buff);
        }

        // Returns true while the card is the copier's: a copy runs or a transfer is under way.
        bool CopyProcess()
        {
            if(scan_reading_) // the header landed: the rest of the tick runs
            {
                ScanStep();
                return false;
            }
            // looper jobs wait for copy, bake, pending change and undo
            if(!copying_ && !req_fifo.IsEmpty()
                && (req_fifo.Front().is_looper == CopyRequest::RamDir::NONE
                    || (!looper_ram.IsCopying() && !looper_ram.IsPending() && !fx_->IsLooperPrinting()
                        && !looper_ram.IsUndoing())))
                Begin();

            if(copying_)
            {
                Step();
                return true;
            }
            if(scanning_ && req_fifo.IsEmpty())
                ScanStep();
            return scan_reading_;
        }

        inline bool IsCopying()  { return copying_ || !req_fifo.IsEmpty(); }

        // Starts the scan that makes every slot's file plain, a slot a tick.
        void StartScan()
        {
            scan_slot_ = scan_bank_ = scan_mode_ = 0;
            scan_reading_ = false;
            scanning_ = true;
        }

        // The card is pulled: drops the copy under way and the scan's read.
        void Detach()
        {
            if(copying_)
                Abort();
            scan_reading_ = false;
        }

        inline bool IsScanning() const { return scanning_; }
        inline bool IsLooperStaged() { return looper_staged; }
        inline bool LooperWasPlaying() { return looper_was_playing; }

        // Reads the slot's header by DMA. Once it is in, an odd header or a footer queues the rewrite.
        void ScanStep()
        {
            const VoiceMode mode = VoiceMode(scan_mode_);
            if(scan_reading_)
            {
                scan_reading_ = false;
                uint32_t at, size;
                const bool plain = !mem_.src->Failed() && chompi::WavData(mem_.in, kHeader, mem_.src->Size(), at, size)
                                   && at == sizeof(WAV_FormatTypeDef) && at + size == mem_.src->Size();
                mem_.src->Close();
                if(!plain)
                    Request(CopyRequest(scan_slot_ + 1, scan_bank_, mode, scan_slot_ + 1, scan_bank_, mode, false,
                                        CopyRequest::RamDir::NONE, CopyRequest::RamDir::NONE));
            }
            else if(fx_->GetFileExists(scan_slot_, scan_bank_, mode)
                    && !Engine::IsKitSlot(scan_slot_ + 1, scan_bank_, mode))
            {
                char fname[32];
                Engine::GetFileNameForSlot(scan_slot_ + 1, scan_bank_, mode, fname);
                if(mem_.src->Open(fname))
                {
                    scan_reading_ = chompi::sd_dma.Read(*mem_.src, 0, mem_.in, kHeader);
                    if(scan_reading_)
                        return;
                    mem_.src->Close();
                }
            }
            if(++scan_slot_ < 14)
                return;
            scan_slot_ = 0;
            if(++scan_bank_ < 5)
                return;
            scan_bank_ = 0;
            scanning_ = ++scan_mode_ < 2;
        }

        struct CopyRequest {

            enum RamDir {
                NONE,
                FROM,
                TO
            };

            CopyRequest(size_t s, size_t b, VoiceMode m, size_t d, size_t db, VoiceMode dm, bool st, RamDir ic, RamDir il)
                : src(s),
                src_bank (b),
                src_mode(m),
                dest(d),
                dest_bank(db),
                dest_mode(dm),
                set(st),
                is_chompi(ic),
                is_looper(il)
            {
            }

            // default
            CopyRequest()
                : src(0),
                src_bank (0),
                src_mode(VoiceMode::JAMMI),
                dest(0),
                dest_bank(0),
                dest_mode(VoiceMode::JAMMI),
                set(false),
                is_chompi(RamDir::NONE),
                is_looper(RamDir::NONE)
            {
            }

            ~CopyRequest() {}

            size_t src;
            size_t src_bank;
            VoiceMode src_mode;
            size_t dest;
            size_t dest_bank;
            VoiceMode dest_mode;
            bool set;
            RamDir is_chompi;
            RamDir is_looper;
            bool append = false;
        };

        // Called from the UI or the card task.
        void Request(const CopyRequest &req)
        {
            ScopedIrqBlocker block;
            req_fifo.PushBack(req);
        }

    private:
        static constexpr uint32_t kHeader = 2048 + 8; // bytes read from a file's head: enough to find the data chunk

        enum class Stage { FILL, FILLED, WRITTEN };

        void Begin()
        {
            copying_ = true; // IsCopying() while it starts
            req = req_fifo.PopFront();
            is_chompi_ram = req.is_chompi;
            is_looper_ram = req.is_looper;
            chompi_ram.SetReadHead(chompi_ram.GetStart());
            if(is_chompi_ram == CopyRequest::RamDir::TO)
                chompi_ram.Reset();
            looper_ram.SetReadHead(looper_ram.LoopStart()); // a save stores what the loop plays
            card_ = false;
            target_[0] = '\0';
            carry_ = 0;

            copying_ = Engine::IsKitSlot(req.src, req.src_bank, req.src_mode) ? CopyKitStart() : CopyStart();
            if(copying_ && is_looper_ram == CopyRequest::RamDir::TO)
                StartLooperLoad();
        }

        // a sample: card or RAM to card or RAM
        bool CopyStart()
        {
            if(req.dest < 15 && Engine::IsKitSlot(req.dest, req.dest_bank, req.dest_mode)) // a sample replaces the kit
            {
                char kit[32];
                Engine::GetFileNameForSlot(req.dest, req.dest_bank, req.dest_mode, kit);
                Engine::KitName(kit);
                chompi::slot_loader.Remove(kit);
            }

            char fname[32];
            if(is_chompi_ram == CopyRequest::RamDir::FROM)
                size_ = chompi_ram.GetSize() * 2;
            else if(is_looper_ram == CopyRequest::RamDir::FROM)
                size_ = looper_ram.LoopLength() * 2;
            else
            {
                Engine::GetFileNameForSlot(req.src, req.src_bank, req.src_mode, fname);
                if(!mem_.src->Open(fname) || !Find(src_at_, size_))
                    return Abort();
            }

            const bool card = ToCard();
            if(card)
            {
                const uint32_t h = sizeof(WAV_FormatTypeDef);
                Engine::GetFileNameForSlot(req.dest, req.dest_bank, req.dest_mode, target_);
                if(!Create(*mem_.dst, target_, h + size_))
                    return Abort();
                chompi::slot_loader.Forget(target_);
                Header(mem_.out, size_);
                carry_ = h;
            }
            if(req.dest < 16)
                fx_->SetFileExists(req.dest - 1, req.dest_bank, req.dest_mode, true);
            Start(card);
            return true;
        }

        // A kit copies as files, the .aif and then its sidecar, and only to another JAMMI slot on the card.
        bool CopyKitStart()
        {
            char from[32];
            Engine::GetFileNameForSlot(req.src, req.src_bank, req.src_mode, from);
            Engine::KitName(from);
            if(req.dest_mode != VoiceMode::JAMMI || req.dest < 1 || req.dest > 14 || !mem_.src->Open(from))
                return false; // nothing changed yet

            Engine::GetFileNameForSlot(req.dest, req.dest_bank, req.dest_mode, target_);
            f_unlink(target_);
            Engine::KitName(target_);
            chompi::slot_loader.Remove(target_);
            fx_->SetFileExists(req.dest - 1, req.dest_bank, req.dest_mode, true);
            chompi::kit_slots[req.dest_bank] |= 1 << req.dest;

            kit_phase_ = 1;
            return CopyRaw();
        }

        // after the .aif comes the sidecar, if there is one
        bool CopyKitNext()
        {
            kit_phase_ = 2;
            char from[32];
            Engine::GetFileNameForSlot(req.src, req.src_bank, req.src_mode, from);
            chompi::KitFileName(from, from, ".json");
            chompi::KitFileName(target_, target_, ".json");
            return mem_.src->Open(from) && CopyRaw();
        }

        // the open source file as it is
        bool CopyRaw()
        {
            size_ = mem_.src->Size();
            src_at_ = 0;
            carry_ = 0;
            if(!Create(*mem_.dst, target_, size_))
                return Abort();
            Start(true);
            return true;
        }

        void Start(bool card)
        {
            card_ = card;
            done_ = dst_at_ = 0;
            stage_ = Stage::FILL;
        }

        void StartLooperLoad()
        {
            const size_t len = size_ / 2;
            looper_staged = false;
            if(!fx_->IsLooperFirstRecording())
            {
                // replace below the playing loop if it fits there, else after it
                looper_ram.StartLoad();
                fx_->LooperStop(fx_->IsLooperPlaying()); // ends an overdub, it would go on over the new loop
                const size_t start = looper_ram.GetStart(), end = looper_ram.End();
                looper_at = !req.append && len <= start ? 0 : end;
                looper_staged = req.append || (len && looper_at + len <= kMaxRamBuffSize);
            }
            if(!looper_staged)
            {
                loop_sync.loading = true; // the empty looper stops filling until OpenFile
                looper_was_playing = fx_->IsLooperPlaying();
                fx_->LooperStop();
                looper_ram.Reset();
                looper_at = 0;
            }
            looper_ram.SetWriteHead(looper_at);
        }

        // a chunk per call at most: returns when a transfer runs, a chunk went into RAM or the copy is done
        void Step()
        {
            using chompi::sd_dma;
            while(copying_)
            {
                switch(stage_)
                {
                    case Stage::FILL:
                    {
                        if(done_ >= size_ && !carry_) // an empty source still writes the header
                        {
                            Finish();
                            return;
                        }
                        n_ = std::min<uint32_t>(chompi::SdDma::kChunk, size_ - done_);
                        stage_ = Stage::FILLED;
                        if(!n_)
                            break;
                        if(is_chompi_ram == CopyRequest::RamDir::FROM || is_looper_ram == CopyRequest::RamDir::FROM)
                        {
                            RamBuffer &ram = is_chompi_ram == CopyRequest::RamDir::FROM ? chompi_ram : looper_ram;
                            ram.BlockRead(reinterpret_cast<int16_t *>(mem_.in), n_ / 2);
                            data_ = mem_.in;
                            break;
                        }
                        const uint32_t pos = src_at_ + done_, skip = pos % 512;
                        data_ = mem_.in + skip;
                        sd_dma.Read(*mem_.src, pos - skip, mem_.in, skip + n_);
                        return;
                    }

                    case Stage::FILLED:
                        if(mem_.src->Failed())
                        {
                            Abort();
                            return;
                        }
                        if(!card_)
                        {
                            int16_t *samples = reinterpret_cast<int16_t *>(data_);
                            if(is_chompi_ram == CopyRequest::RamDir::TO)
                            {
                                chompi::slot_loader.Reserve(chompi_ram.GetWriteHead() + n_ / 2);
                                chompi_ram.BlockWrite(samples, n_ / 2);
                            }
                            else if(!looper_staged || looper_ram.IsLoading()) // not after a reset
                                looper_ram.BlockWrite(samples, n_ / 2, !looper_staged);
                            done_ += n_;
                            stage_ = Stage::FILL;
                            return;
                        }
                        memcpy(mem_.out + carry_, data_, n_);
                        write_ = Whole(carry_ + n_);
                        stage_ = Stage::WRITTEN;
                        sd_dma.Write(*mem_.dst, dst_at_, mem_.out, write_);
                        return;

                    case Stage::WRITTEN:
                        if(mem_.dst->Failed())
                        {
                            Abort();
                            return;
                        }
                        dst_at_ += write_;
                        carry_ += n_ - write_;
                        memmove(mem_.out, mem_.out + write_, carry_);
                        done_ += n_;
                        stage_ = Stage::FILL;
                        break;
                }
            }
        }

        // the target is a slot on the card, not the recording or the looper
        bool ToCard() const
        {
            return is_chompi_ram != CopyRequest::RamDir::TO && is_looper_ram != CopyRequest::RamDir::TO;
        }

        // the bytes a write takes: whole sectors, everything with the last chunk
        uint32_t Whole(uint32_t bytes) const { return done_ + n_ >= size_ ? bytes : bytes & ~511u; }

        void Finish()
        {
            Close();
            if(card_ && !Rename(target_))
                fx_->UpdateFileExists(req.dest - 1, req.dest_bank, req.dest_mode);

            if(kit_phase_ == 1 && CopyKitNext())
                return;
            kit_phase_ = 0;

            if(is_looper_ram == CopyRequest::RamDir::TO && looper_staged && looper_ram.IsLoading()) // not reset meanwhile
            {
                const size_t end = looper_ram.GetWriteHead();
                const size_t start = req.append ? looper_ram.GetStart() : looper_at;
                if(req.append)
                    looper_ram.FadeJoin(looper_at, end);
                looper_ram.FinishLoad(start, end - start);
            }

            copying_ = false;

            if(req.set)
            {
                fx_->SetBank(req.dest_bank);
                fx_->SetVoiceMode(req.dest_mode);
                fx_->SetVoiceSlot(req.dest, false);
            }

            fx_->SetAllCopyOccurred();
        }

        // the target files stay as they were
        bool Abort()
        {
            Close();
            if(target_[0])
            {
                char tmp[40];
                Tmp(tmp, target_);
                f_unlink(tmp);
            }
            if(ToCard())
                fx_->UpdateFileExists(req.dest - 1, req.dest_bank, req.dest_mode);
            if(is_looper_ram == CopyRequest::RamDir::TO && looper_staged)
                looper_ram.FinishLoad(looper_at, 0); // nothing loaded, the playing loop goes on
            kit_phase_ = 0;
            copying_ = false;
            fx_->SetAllCopyOccurred();
            return false;
        }

        void Close()
        {
            mem_.src->Close();
            mem_.dst->Close();
        }

        static void Tmp(char *tmp, const char *name)
        {
            strcpy(tmp, name);
            strcat(tmp, ".tmp");
        }

        // the target as a .tmp, size bytes allocated
        bool Create(chompi::SdFile &file, const char *target, uint32_t size)
        {
            char tmp[40];
            Tmp(tmp, target);
            return file.Open(tmp, true, size);
        }

        static bool Rename(const char *target)
        {
            char tmp[40];
            Tmp(tmp, target);
            f_unlink(target);
            return f_rename(tmp, target) == FR_OK;
        }

        void Header(uint8_t *to, uint32_t size)
        {
            file_header.SubCHunk2Size = size;
            file_header.FileSize = sizeof(file_header) + size;
            memcpy(to, &file_header, sizeof(file_header));
        }

        // the data chunk of the open source: where it starts and its size in whole frames
        bool Find(uint32_t &at, uint32_t &size)
        {
            FIL &fil = mem_.src->fil;
            UINT br;
            return f_lseek(&fil, 0) == FR_OK && f_read(&fil, mem_.in, kHeader, &br) == FR_OK
                   && chompi::WavData(mem_.in, br, mem_.src->Size(), at, size);
        }

        Memory mem_;
        bool copying_;
        bool scanning_ = false, scan_reading_ = false;
        uint8_t scan_slot_ = 0, scan_bank_ = 0, scan_mode_ = 0;
        // loading next to the playing loop, which keeps playing until the loop point
        bool looper_staged = false;
        bool looper_was_playing = false;
        uint8_t kit_phase_ = 0; // copying a kit: 1 the .aif, 2 its sidecar
        WAV_FormatTypeDef file_header;

        Stage stage_;
        bool card_; // writing to the card
        char target_[32];
        uint8_t *data_;
        uint32_t size_, done_, n_, src_at_; // bytes; src_at_: where the data starts in the source
        uint32_t dst_at_, carry_, write_;

        // RAM IO
        CopyRequest::RamDir is_chompi_ram;
        CopyRequest::RamDir is_looper_ram;
        RamBuffer chompi_ram;
        RamBuffer looper_ram;
        size_t looper_at = 0;

        FIFO<CopyRequest, 16> req_fifo;
        CopyRequest req;

        Engine* fx_;
};
} // namespace daisy
#pragma GCC pop_options
