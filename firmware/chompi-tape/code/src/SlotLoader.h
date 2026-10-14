#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once

#include <stdio.h>
#include <algorithm>
#include "fatfs.h"
#include "SdDma.h"
#include "Wav.h"
#include "RamBuffer.h"
#include "AiffKit.h"

namespace chompi
{
    static constexpr size_t kSlotBounce = SdDma::kBuffer;
    static constexpr uint32_t kKitSaveDelay = 1000;     // ms after the last edit

    extern daisy::RamBufferMemory slot_buff; // the loaded slot, stereo, played from RAM

    // The same name with another extension: a kit's .json sidecar holds its slice edits, the .aif stays untouched
    inline void KitFileName(char *name, const char *from, const char *ext)
    {
        if(name != from)
            strcpy(name, from);
        const size_t n = strlen(name);
        strcpy(name + (n < 4 ? n : n - 4), ext);
    }

    // Loads the JAMMI slot into slot_buff by DMA, a chunk per transfer: an OP-1 kit or other .aif, or a .wav that fits
    // (else it streams), and keeps a kit's slice edits in the sidecar. Everything here runs on the SD timer, except
    // Request, Loaded, Edit, Forget and Reserve.
    // The SD DMA can't reach DTCM and wants aligned addresses, so reads go through a bounce buffer in SDRAM.
    // slot_buff is the top of the recording's memory; the recording growing near it drops it.
    class SlotLoader
    {
      public:
        void Init(SdFile *file, uint8_t *bounce, int16_t *pool, size_t pool_size, const daisy::RamBufferMemory *recording)
        {
            pool_ = pool;
            pool_size_ = pool_size;
            recording_ = recording;
            evict_ = false;
            slot_buff.mem = pool + pool_size;
            file_ = file;
            fil_ = &file->fil;
            bounce_ = bounce;
            name_[0] = want_[0] = '\0';
            pending_ = loading_ = reading_ = dirty_ = failed_ = false;
            frames_ = done_ = size_ = 0;
            last_slice_ = -1;
        }

        // from the UI: load the slot unless it is there already
        void Request(const char *name)
        {
            pending_ = false;
            __DMB();
            strcpy(want_, name);
            __DMB();
            pending_ = true;
        }

        uint32_t Frames() const { return slot_buff.length / 2; }
        // the requested slot is in RAM
        bool Loaded() const
        {
            return !pending_ && !loading_ && !evict_ && slot_buff.length && name_[0] && !strcmp(want_, name_);
        }

        // the recording writes up to end (samples): the cache goes when it gets near
        void Reserve(size_t end)
        {
            if(end + kKeepFree >= CacheLow())
            {
                evict_ = true;
                slot_buff.length = 0;
            }
        }
        bool Busy() const { return pending_ || loading_; }
        bool Failed() const { return failed_; } // the file isn't one this can play

        bool HasSlice(int slice) const
        {
            return slot_buff.length && slice >= 0 && slice < int(kKitSlices)
                   && slot_info.end[slice] > slot_info.start[slice] && slot_info.start[slice] < Frames();
        }
        int LastSlice() const { return last_slice_; }

        // the slice under a key, -1 for none; it is the one the knobs edit next
        int Slice(int slice)
        {
            if(!HasSlice(slice))
                return -1;
            last_slice_ = slice;
            return slice;
        }

        // from the UI: move the last played slice's start or end, saved to the sidecar a little later
        void Edit(bool end, int frames, uint32_t now)
        {
            const int slice = last_slice_;
            if(slice < 0 || !slot_buff.length)
                return;
            const int32_t min_len = int32_t(slot_info.rate * .005f); // 5 ms
            int32_t s = slot_info.start[slice], e = slot_info.end[slice];
            if(end)
                e = std::max(s + min_len, std::min(e + frames, int32_t(Frames())));
            else
                s = std::max(int32_t(0), std::min(s + frames, e - min_len));
            slot_info.start[slice] = s;
            slot_info.end[slice] = e;
            dirty_ = true;
            edit_time_ = now;
        }

        // the card goes away for a while: a load stops, edits are saved, it loads again after. The card must be free
        void Release()
        {
            Drop();
            name_[0] = '\0';
        }

        // The card is pulled: a load under way is lost and waits for the card; what is loaded stays.
        void Detach()
        {
            if(!loading_)
                return;
            file_->Close();
            loading_ = reading_ = false;
            slot_buff.length = 0;
            name_[0] = '\0';
            pending_ = true;
        }

        // its files changed: load it again next time, a load under way stops
        void Forget(const char *name)
        {
            if(!strcmp(name, name_))
            {
                name_[0] = '\0';
                dirty_ = false;
            }
        }

        // SD timer: delete a kit and its sidecar
        void Remove(const char *aif)
        {
            char json[32];
            KitFileName(json, aif, ".json");
            Forget(aif);
            f_unlink(aif);
            f_unlink(json);
        }

        // SD timer
        void Step()
        {
            if(evict_)
            {
                Drop();
                name_[0] = '\0';
                evict_ = false;
            }
            if(pending_)
                Open();
            if(loading_)
                Load();
        }

        // SD timer, while nothing plays: the edits go to the sidecar, written next to it and renamed over
        void Save(uint32_t now)
        {
            if(dirty_ && !loading_ && name_[0] && now - edit_time_ >= kKitSaveDelay)
                Write();
        }

      private:
        static constexpr size_t kKeepFree = 48000 * 2; // samples between the recording and the cache

        size_t CacheLow() const
        {
            return (loading_ || slot_buff.length) && !evict_ ? size_t(slot_buff.mem - pool_) : pool_size_;
        }

        void Write()
        {
            dirty_ = false;

            char *json = reinterpret_cast<char *>(bounce_);
            int n = sprintf(json, "{\"size\":%lu,\"start\":[", (unsigned long)size_);
            for(size_t i = 0; i < kKitSlices; i++)
                n += sprintf(json + n, i ? ",%lu" : "%lu", (unsigned long)slot_info.start[i]);
            n += sprintf(json + n, "],\"end\":[");
            for(size_t i = 0; i < kKitSlices; i++)
                n += sprintf(json + n, i ? ",%lu" : "%lu", (unsigned long)slot_info.end[i]);
            n += sprintf(json + n, "]}");

            char tmp[32];
            KitFileName(tmp, name_, ".tmp");
            KitFileName(json_, name_, ".json");
            UINT bw = 0;
            if(f_open(fil_, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
                return;
            const bool ok = f_write(fil_, json, n, &bw) == FR_OK && bw == UINT(n);
            f_close(fil_);
            if(ok)
            {
                f_unlink(json_);
                f_rename(tmp, json_);
            }
        }

        bool Read(uint32_t pos, void *dst, uint32_t n)
        {
            UINT br = 0;
            const uint32_t pad = pos & 3;
            if(n + pad > kSlotBounce || f_lseek(fil_, pos) != FR_OK || f_read(fil_, bounce_ + pad, n, &br) != FR_OK
               || br != n)
                return false;
            memcpy(dst, bounce_ + pad, n);
            return true;
        }

        void Open()
        {
            pending_ = false;
            if(name_[0] && !strcmp(want_, name_)) // loaded or loading
                return;
            Drop();
            last_slice_ = -1;
            failed_ = false;
            strcpy(name_, want_);
            if(!file_->Open(name_))
            {
                name_[0] = '\0';
                failed_ = true;
                return;
            }

            size_ = file_->Size();
            if(!IsAiffName(name_)) // a .wav is 48 kHz stereo on the card, one that doesn't fit streams
            {
                if(!Wav())
                {
                    file_->Close();
                    name_[0] = '\0';
                    return;
                }
            }
            else // the JSON scratch is the top of the pool, the audio comes after the parse
            {
                auto read = [this](uint32_t pos, void *dst, uint32_t n) { return Read(pos, dst, n); };
                char *scratch = reinterpret_cast<char *>(pool_ + pool_size_) - kSlotBounce;
                if(Room() * 4 < kSlotBounce || !ParseAiff(read, size_, slot_info, scratch, kSlotBounce))
                {
                    Fail();
                    return;
                }
            }
            frames_ = std::min<uint32_t>(slot_info.frames, Room());
            slot_buff.mem = pool_ + pool_size_ - frames_ * 2;
            __DMB(); // before loading_ makes CacheLow use it
            done_ = 0;
            loading_ = true;
        }

        // a chunk per transfer, converted to stereo when it is in
        void Load()
        {
            const AiffInfo &a = slot_info;
            const uint32_t frame = a.channels * 2;
            if(reading_)
            {
                reading_ = false;
                if(file_->Failed())
                {
                    Fail();
                    return;
                }
                const uint8_t *src = bounce_ + skip_;
                int16_t *dst = slot_buff.mem + done_ * 2;
                for(uint32_t i = 0; i < chunk_; i++)
                {
                    const uint8_t *l = src + i * frame, *r = l + frame - 2;
                    dst[2 * i] = a.little_endian ? l[0] | l[1] << 8 : l[0] << 8 | l[1];
                    dst[2 * i + 1] = a.little_endian ? r[0] | r[1] << 8 : r[0] << 8 | r[1];
                }
                done_ += chunk_;
                if(evict_) // dropped meanwhile, Step cleans up
                    return;
                if(done_ >= frames_)
                {
                    file_->Close();
                    loading_ = false;
                    if(IsAiffName(name_))
                        ReadSidecar();
                    __DMB(); // the voices must not see the length before the audio
                    slot_buff.length = frames_ * 2;
                    return;
                }
            }
            if(!name_[0]) // forgotten
            {
                file_->Close();
                loading_ = false;
                return;
            }
            chunk_ = std::min<uint32_t>(frames_ - done_, SdDma::kChunk / frame);
            const uint32_t pos = a.data_offset + done_ * frame;
            skip_ = pos % 512;
            reading_ = sd_dma.Read(*file_, pos - skip_, bounce_, skip_ + chunk_ * frame);
            if(!reading_)
                Fail();
        }

        // slice edits from an earlier session, if they belong to this file; a .tmp is a save cut short
        void ReadSidecar()
        {
            KitFileName(json_, name_, ".json");
            if(f_open(fil_, json_, FA_READ) != FR_OK)
            {
                KitFileName(json_, name_, ".tmp");
                if(f_open(fil_, json_, FA_READ) != FR_OK)
                    return;
            }
            const uint32_t n = f_size(fil_);
            char *json = reinterpret_cast<char *>(bounce_);
            UINT br = 0;
            const bool ok = n < kSlotBounce && f_read(fil_, json, n, &br) == FR_OK && br == n;
            f_close(fil_);
            int32_t size;
            if(!ok || JSON_Validate(json, n) != JSONSuccess || !aiff::Number(json, n, "size", 4, size)
               || uint32_t(size) != size_)
                return;
            AiffInfo edits = slot_info;
            for(uint32_t i = 0; i < kKitSlices; i++)
            {
                int32_t s, e;
                if(!aiff::ArrayValue(json, n, "start", i, s) || !aiff::ArrayValue(json, n, "end", i, e))
                    return;
                edits.start[i] = s;
                edits.end[i] = e;
            }
            slot_info = edits;
        }

        // the data chunk of a .wav that fits
        bool Wav()
        {
            uint8_t head[256];
            const uint32_t n = std::min<uint32_t>(size_, sizeof(head));
            uint32_t at, bytes;
            if(!Read(0, head, n) || !WavData(head, n, size_, at, bytes))
                return false;
            slot_info = AiffInfo();
            slot_info.data_offset = at;
            slot_info.frames = bytes / 4;
            slot_info.channels = 2;
            slot_info.little_endian = true;
            slot_info.rate = 48000.f;
            return slot_info.frames && slot_info.frames <= Room();
        }

        // frames that fit above the recording
        uint32_t Room() const
        {
            const size_t used = recording_->length + kKeepFree;
            return used < pool_size_ ? (pool_size_ - used) / 2 : 0;
        }

        // stops a load and saves the kit's edits, silent until the next load
        void Drop()
        {
            file_->Close();
            loading_ = reading_ = false;
            if(dirty_)
                Write();
            slot_buff.length = 0;
        }

        void Fail()
        {
            file_->Close();
            loading_ = reading_ = false;
            name_[0] = '\0';
            failed_ = true;
        }

        int16_t *pool_;
        size_t pool_size_;
        const daisy::RamBufferMemory *recording_;
        volatile bool evict_;
        SdFile *file_;
        FIL *fil_; // the same file's FIL, for the small reads and the sidecar
        uint8_t *bounce_;
        char name_[32], want_[32], json_[32];
        volatile bool pending_;
        bool loading_, reading_, failed_;
        volatile bool dirty_;
        int8_t last_slice_;
        uint32_t frames_, done_, size_, edit_time_, chunk_, skip_;
    };

    extern SlotLoader slot_loader;
}

#pragma GCC pop_options
