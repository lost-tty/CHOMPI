#pragma once

#include <stdint.h>
#include <algorithm>
#include "PageCache.h"

namespace chompi
{
    class SdFile;

    // Audio in pages of the cache: a sample file, read in its format, or scratch, 16-bit stereo written in RAM and
    // spilled to a file of its own. Who reads keeps a Tlb; a page that isn't in reads null.
    class Tape
    {
      public:
        static constexpr uint32_t kShift = 16;
        static constexpr uint32_t kMask = (1u << kShift) - 1;
        static constexpr uint32_t kSamples = PageCache::kBytes / 2;
        static constexpr uint32_t kMax = 65535;                      // pages: the file stays below 4 GB

        enum class Kind : uint8_t { kNone, kFile, kScratch };

        struct Format
        {
            uint32_t offset; // bytes before the first frame
            uint32_t frames;
            uint8_t channels;
            bool little;
            float rate;
        };

        // the pages a reader looked up last, valid while the cache's epoch stays
        struct Tlb
        {
            struct Entry
            {
                uint32_t page, epoch;
                uint8_t *base;
            } e[4];
            void Flush()
            {
                for(Entry &x : e)
                    x = {~0u, 0, nullptr};
            }
        };

        // table: kMax + 1 entries in SDRAM
        void Init(PageCache *cache, uint8_t id, uint16_t *table, SdFile *file)
        {
            cache_ = cache;
            id_ = id;
            table_ = table;
            file_ = file;
            kind_ = Kind::kNone;
            fmt_ = Format();
            std::fill(table_, table_ + kMax + 1, uint16_t(0));
            seq_ = 1;
            read_.Flush();
            write_.Flush();
        }

        void InitScratch()
        {
            kind_ = Kind::kScratch;
            fmt_ = {0, 0, 2, true, 48000.f};
        }

        uint8_t Id() const { return id_; }
        Kind GetKind() const { return kind_; }
        const Format &Fmt() const { return fmt_; }
        SdFile &File() const { return *file_; }

        // ---- reading and writing: the audio thread

        const uint8_t *Bytes(uint32_t pos, Tlb &t) const
        {
            uint8_t *const base = Page(pos >> kShift, t);
            return base ? base + (pos & kMask) : nullptr;
        }

        bool Frame(uint32_t f, Tlb &t, float &l, float &r) const
        {
            const uint32_t pos = fmt_.offset + f * (fmt_.channels * 2u);
            const uint8_t *p = Bytes(pos, t);
            if(!p)
                return false;
            l = Sample(p);
            if(fmt_.channels == 1)
            {
                r = l;
                return true;
            }
            p = (pos + 2) & kMask ? p + 2 : Bytes(pos + 2, t); // a frame may straddle pages
            if(!p)
                return false;
            r = Sample(p);
            return true;
        }

        // scratch: one reader and one writer in the owner's thread; another reader brings its own Tlb
        int16_t *Read(size_t pos) { return Samples(pos, read_, false); }
        int16_t *Write(size_t pos) { return Samples(pos, write_, true); }
        const int16_t *Read(size_t pos, Tlb &t) { return Samples(pos, t, false); }

        // scratch, any thread: the samples from pos on within its page, at most n, n set to how many
        int16_t *Run(size_t pos, size_t &n, bool write)
        {
            const int b = Slot(pos / kSamples);
            if(b < 0)
                return nullptr;
            if(write)
                Touch(b);
            n = std::min<size_t>(n, kSamples - pos % kSamples);
            return reinterpret_cast<int16_t *>(cache_->Data(b)) + pos % kSamples;
        }

        bool Ready(size_t pos, size_t n) const
        {
            for(uint32_t p = pos / kSamples; n && p <= (pos + n - 1) / kSamples; p++)
                if(Slot(p) < 0)
                    return false;
            return true;
        }

        // ---- the card side

        int Slot(uint32_t page) const { return page < kMax ? int(table_[page]) - 1 : -1; }
        bool In(uint32_t page) const { return Slot(page) >= 0; }
        void Map(uint32_t page, int b) { table_[page] = b + 1; }
        void Unmap(uint32_t page, int b) // b goes: a block still coming in isn't mapped
        {
            if(Slot(page) == b)
                table_[page] = 0;
        }
        uint8_t Seq() const { return seq_; }

        // A file: what it is. Another file: what was read of the one before, and a read of it under way, are the
        // seq before's.
        void SetFile(const Format &f, bool same)
        {
            fmt_ = f;
            kind_ = Kind::kFile;
            if(!same)
            {
                if(++seq_ == 0)
                    seq_ = 1;
                cache_->epoch = cache_->epoch + 1;
            }
        }
        void SetNone() { kind_ = Kind::kNone; }

        // a block written: what it holds is the current content, and has to go to the card before it goes
        void Touch(int b)
        {
            PageCache::Block &k = (*cache_)[b];
            k.seq = seq_;
            k.gen = k.gen + 1;
        }

      private:
        float Sample(const uint8_t *p) const
        {
            return int16_t(fmt_.little ? p[0] | p[1] << 8 : p[0] << 8 | p[1]) * (1.f / 32768.f);
        }

        uint8_t *Page(uint32_t page, Tlb &t) const
        {
            Tlb::Entry &e = t.e[page & 3];
            const uint32_t epoch = cache_->epoch;
            if(e.page != page || e.epoch != epoch)
            {
                const int b = Slot(page);
                if(b < 0)
                    return nullptr;
                e = {page, epoch, cache_->Data(b)};
            }
            return e.base;
        }

        // A write looks its block up again after the card side started writing it out (the epoch changes), so
        // the block counts as written again.
        int16_t *Samples(size_t pos, Tlb &t, bool write)
        {
            const uint32_t page = pos / kSamples;
            Tlb::Entry &e = t.e[page & 3];
            const uint32_t epoch = cache_->epoch;
            if(e.page != page || e.epoch != epoch)
            {
                const int b = Slot(page);
                if(b < 0)
                    return nullptr;
                if(write)
                    Touch(b);
                e = {page, epoch, cache_->Data(b)};
            }
            return reinterpret_cast<int16_t *>(e.base) + pos % kSamples;
        }

        PageCache *cache_;
        uint16_t *table_; // block + 1 of each page, 0 when it isn't in
        SdFile *file_;
        Kind kind_;
        Format fmt_;
        uint8_t id_;
        volatile uint8_t seq_;
        Tlb read_, write_;
    };
}
