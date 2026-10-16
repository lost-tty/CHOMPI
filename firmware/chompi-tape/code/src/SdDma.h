#pragma GCC push_options
#pragma GCC optimize ("Os")
#pragma once

#include <algorithm>
#include "daisy.h"
#include "fatfs.h"

extern SD_HandleTypeDef hsd1;

namespace chompi
{
    // A file and the card sectors it is on, mapped on the first transfer. In SDRAM: FatFS reads into the FIL by DMA.
    class SdFile
    {
      public:
        // write: created with size bytes allocated, the content is undefined until written
        bool Open(const char *name, bool write = false, FSIZE_t size = 0)
        {
            Close();
            if(f_open(&fil, name, write ? FA_CREATE_ALWAYS | FA_WRITE | FA_READ : FA_READ) != FR_OK)
                return false;
            open_ = true;
            if(write && (f_lseek(&fil, size) != FR_OK || f_tell(&fil) != size || f_sync(&fil) != FR_OK))
            {
                Close();
                return false;
            }
            return true;
        }

        void Close()
        {
            if(open_)
            {
                fil.flag &= ~kDirty; // FatFS's sector buffer may be older than what DMA wrote
                f_close(&fil);
            }
            open_ = mapped_ = unmappable_ = failed_ = false;
        }

        bool IsOpen() const { return open_; }
        bool Failed() const { return failed_; } // the last transfer

        // the card sectors, false for a file in more fragments than the map holds
        bool Map()
        {
            if(!mapped_ && !unmappable_)
            {
                fil.cltbl = map_;
                map_[0] = kMap;
                mapped_ = f_lseek(&fil, CREATE_LINKMAP) == FR_OK;
                unmappable_ = !mapped_;
                if(unmappable_)
                    fil.cltbl = nullptr;
            }
            return mapped_;
        }

        FSIZE_t Size() const { return f_size(&fil); }

        // the sector at offset (sector aligned) and how many follow it on the card, at most max
        bool Run(FSIZE_t offset, uint32_t max, DWORD &sector, uint32_t &count) const
        {
            const FATFS *fs = fil.obj.fs;
            const DWORD cluster = DWORD(fs->csize) * 512;
            DWORD index = offset / cluster;
            for(const DWORD *m = map_ + 1; *m; m += 2)
            {
                if(index < m[0])
                {
                    const DWORD first = m[1] + index, in = offset % cluster / 512;
                    if(first < 2 || m[1] + m[0] > fs->n_fatent) // outside the volume
                        return false;
                    sector = fs->database + (first - 2) * fs->csize + in;
                    count = std::min<uint32_t>(max, (m[0] - index) * fs->csize - in);
                    return true;
                }
                index -= m[0];
            }
            return false;
        }

        FIL fil;

      private:
        friend class SdDma;
        static constexpr int kMap = 128;     // DWORDs: 63 fragments
        static constexpr BYTE kDirty = 0x80; // ff.c's FA_DIRTY
        DWORD map_[kMap];
        bool open_, mapped_, unmappable_, failed_; // no initialisers: in SDRAM, which is zeroed in main, not before
    };

    // One transfer at a time between a file and SDRAM, by DMA in as few runs as the file's fragments allow, or of
    // raw sectors. Start it from the card task and poll Busy() there; nothing else may use the card until it returns
    // false. Then Failed(), the file's or for sectors this one's, tells how it went.
    // buf: 32-byte aligned, room for bytes rounded up to sectors. offset: sector aligned.
    class SdDma
    {
      public:
        static constexpr size_t kChunk = 32768;         // bytes per transfer
        static constexpr size_t kBuffer = kChunk + 512; // for a chunk that starts inside a sector

        void Init() { active_ = false; }

        // false if it failed to start
        bool Read(SdFile &file, FSIZE_t offset, void *buf, uint32_t bytes) { return Start(file, offset, buf, bytes, false); }
        bool Write(SdFile &file, FSIZE_t offset, const void *buf, uint32_t bytes)
        {
            return Start(file, offset, const_cast<void *>(buf), bytes, true);
        }

        bool Sectors(uint32_t sector, uint32_t count, void *buf, bool write)
        {
            file_ = nullptr;
            sector_ = sector;
            return Start(buf, count * 512, write);
        }

        // a transfer started and not yet seen to end by Busy
        bool Active() const { return active_; }
        bool Failed() const { return failed_; }

        // advances the transfer; false once it is done or failed
        bool Busy()
        {
            if(!active_)
                return false;
            const bool timeout = daisy::System::GetNow() - started_ > kTimeout;
            if(HAL_SD_GetState(&hsd1) != HAL_SD_STATE_READY || HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER)
            {
                if(!timeout)
                    return true;
                HAL_SD_Abort(&hsd1);
                return Fail();
            }
            if(hsd1.ErrorCode != HAL_SD_ERROR_NONE)
                return Fail();
            if(!write_)
                SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(buf_), run_ * 512);
            buf_ += run_ * 512;
            offset_ += run_ * 512;
            sector_ += run_;
            bytes_ -= std::min<uint32_t>(bytes_, run_ * 512);
            return Next();
        }

      private:
        static constexpr uint32_t kTimeout = 500; // ms

        bool Start(SdFile &file, FSIZE_t offset, void *buf, uint32_t bytes, bool write)
        {
            file_ = &file;
            offset_ = offset;
            file.failed_ = false;
            return Start(buf, bytes, write);
        }

        bool Start(void *buf, uint32_t bytes, bool write)
        {
            buf_ = static_cast<uint8_t *>(buf);
            bytes_ = bytes;
            write_ = write;
            failed_ = false;
            active_ = true;
            return Next() || !failed_;
        }

        // starts the next run; false when there is none
        bool Next()
        {
            if(!bytes_)
            {
                active_ = false;
                return false;
            }
            DWORD sector = sector_;
            uint32_t count = bytes_ / 512;
            if(file_ && (!file_->Map() || !file_->Run(offset_, (bytes_ + 511) / 512, sector, count)))
                return FatFs();
            if(write_)
                SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(buf_), count * 512);
            else
                SCB_CleanInvalidateDCache_by_Addr(reinterpret_cast<uint32_t *>(buf_), count * 512);
            hsd1.ErrorCode = HAL_SD_ERROR_NONE;
            const HAL_StatusTypeDef res = write_ ? HAL_SD_WriteBlocks_DMA(&hsd1, buf_, sector, count)
                                                 : HAL_SD_ReadBlocks_DMA(&hsd1, buf_, sector, count);
            if(res != HAL_OK)
                return Fail();
            run_ = count;
            started_ = daisy::System::GetNow();
            return true;
        }

        // an unmapped file goes through FatFS, blocking
        bool FatFs()
        {
            UINT done = 0;
            const uint32_t bytes = write_ ? std::min<FSIZE_t>(bytes_, file_->Size() - offset_) : bytes_;
            FRESULT res = f_lseek(&file_->fil, offset_);
            if(res == FR_OK)
                res = write_ ? f_write(&file_->fil, buf_, bytes, &done) : f_read(&file_->fil, buf_, bytes, &done);
            if(res == FR_OK && write_)
                res = f_sync(&file_->fil); // Close drops FatFS's sector buffer
            if(res != FR_OK || (write_ && done != bytes))
                return Fail();
            active_ = false;
            return false;
        }

        bool Fail()
        {
            if(file_)
                file_->failed_ = true;
            failed_ = true;
            active_ = false;
            return false;
        }

        SdFile *file_; // null for sectors
        FSIZE_t offset_;
        uint32_t sector_;
        uint8_t *buf_;
        uint32_t bytes_, run_, started_;
        bool write_, active_, failed_;
    };

    extern SdDma sd_dma;
}

#pragma GCC pop_options
