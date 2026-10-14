#pragma once
#include "daisy_seed.h"
#include "daisysp.h"

namespace daisy
{

    static const size_t kMaxRamBuffSize = 31694848 / 2;
    // ~50ms
    static const size_t kMinLoopSize = 4800;
    static const size_t kLoopFadeSize = 480;
    static constexpr float kLoopFadeStep = .00464f;
    static const size_t kCopyStepSize = 512;
    // a recording keeps this much before and after, the start and end points sit inside. The ring past the
    // buffer holds that and the half second a held key takes before it records
    static const size_t kRecRoll = 48000;
    static const size_t kRecRing = 2 * kRecRoll;
    // a sound's onset before the level crosses the threshold, kept inside the start point
    static const size_t kRecOnset = 288;
    // a recording waits this long after arming before the threshold counts, the key is still noisy
    static const size_t kRecSettle = 4800;
    // the fills before a snapped start walk up the buffer, keeping the loop's lead-in below it
    static const size_t kMaxSyncFill = 48000 * 2 * 8;
    // undo audio is kept at the end of the buffer
    static const size_t kUndoChunk = kCopyStepSize;
    static const size_t kUndoMaxChunks = kMaxRamBuffSize / 2 / kUndoChunk + 1;
    static const size_t kUndoScan = 64;
    // total size = 31694848, which goes evenly into 8K, which means SDRAM page alignment

    struct UndoMemory
    {
        uint32_t saved[kUndoMaxChunks / 32 + 1];
        size_t lo, store;
        size_t chunks;
        bool taking;
        bool pending;
        size_t next, from, loop_lo, loop_n;
        bool rev;
    };

    // just a buffer with its length
    struct RamBufferMemory
    {
        void Init(int16_t* m, UndoMemory* u = nullptr)
        {
            mem = m;
            undo = u;
            if(undo)
                *undo = UndoMemory(); // not cleared at startup
        }

        int16_t* mem;
        size_t length;

        size_t start = 0, base = 0;

        volatile size_t copy_lo = 0, copy_hi = 0;

        size_t halve_len[12];
        uint8_t halve_count = 0;
        int8_t length_exp = 0;
        // The next loop bounds apply by kind: a length change keeps the phase, a loaded loop waits for the loop
        // point and restarts, a cut keeps the heads where they are.
        enum class Pending : uint8_t { kLength, kRestart, kCut };
        Pending pending = Pending::kLength;
        bool loading = false;

        size_t pending_len = 0, pending_start = 0;
        // rec_start and rec_len bound the recording the loop is cut from; cut_start and cut_end hold the cut's
        // points, in samples of it.
        size_t rec_start = 0, rec_len = 0;
        size_t cut_start = 0, cut_end = 0;

        UndoMemory* undo = nullptr;

        void Clear()
        {
            std::fill(&mem[0], &mem[kMaxRamBuffSize], 0);
        }
    };

    // keeps track of its own read and write heads
    class RamBuffer
    {
    public:
        void Init(RamBufferMemory* b) { buff = b; }

        inline void Reset() { FillFrom(0); }

        // play other memory, without clearing it
        void Bind(RamBufferMemory* b)
        {
            buff = b;
            read_head = write_head = 0;
        }
        inline RamBufferMemory* Memory() const { return buff; }

        // the loop starts here: what sits below stays, it is the audio before the loop point
        void FillFrom(size_t pos)
        {
            ClearLengthChanges(); // first, it applies a pending change
            buff->length = 0;
            buff->start = buff->base = read_head = write_head = pos;
            buff->copy_lo = buff->copy_hi = 0;
            buff->rec_len = 0;
            buff->cut_start = buff->cut_end = pos;
        }

        void MarkRecorded() { SetRecorded(buff->start, buff->length); }
        inline size_t RecStart() const { return buff->rec_start; }
        inline size_t RecLen() const { return buff->rec_len; }

        // Puts a point at pos, or moves it by n samples, within the recording. False when it didn't change.
        bool SetCut(int k, size_t pos)
        {
            if(!buff->rec_len)
                return false;
            size_t &pt = k ? buff->cut_end : buff->cut_start;
            const size_t lo = k ? buff->cut_start + kMinLoopSize : buff->rec_start;
            const size_t hi = k ? buff->rec_start + buff->rec_len : buff->cut_end - kMinLoopSize;
            if(lo > hi)
                return false;
            const size_t v = std::min(hi, std::max(lo, pos)) & ~size_t(1);
            if(v == pt)
                return false;
            pt = v;
            return true;
        }

        bool MoveCut(int k, int32_t n)
        {
            const size_t pt = k ? buff->cut_end : buff->cut_start;
            return SetCut(k, size_t(std::max<int32_t>(0, int32_t(pt) + n)));
        }

        inline size_t Cut(int k) const { return k ? buff->cut_end : buff->cut_start; }

        inline size_t CutLen() const { return buff->cut_end - buff->cut_start; }

        // Returns the point as a fraction of the recording, for its LED.
        float CutFraction(int k) const
        {
            const size_t pt = k ? buff->cut_end : buff->cut_start;
            return buff->rec_len ? float(double(pt - buff->rec_start) / buff->rec_len) : k ? 1.f : 0.f;
        }

        void BlockRead(int16_t* copy_buff, size_t size)
        {
            const size_t end = std::min(LoopEnd(), kMaxRamBuffSize);
            size = read_head < end ? std::min(size, end - read_head) : 0;

            std::copy(&buff->mem[read_head], &buff->mem[read_head + size], copy_buff);
            read_head += size;
        }

        void StereoRead(int16_t* l, int16_t* r, bool rev)
        {
            const size_t new_read_head = read_head + 2;
            if(!rev && new_read_head <= End() && new_read_head <= kMaxRamBuffSize)
            {
                *l = buff->mem[read_head];
                *r = buff->mem[read_head + 1];
                read_head = new_read_head;
            }
            // the memory may have been emptied or replaced by a shorter one
            else if(rev && read_head >= buff->start + 2 && read_head <= End())
            {
                *l = buff->mem[read_head - 2];
                *r = buff->mem[read_head - 1];
                read_head -= 2;
            }
            else
            {
                *l = *r = 0;
            }
        }

        void BlockWrite(int16_t* copy_buff, size_t size, bool set_len = true)
        {
            if(write_head + size > kMaxRamBuffSize)
                size = kMaxRamBuffSize - write_head;

            std::copy(copy_buff, &copy_buff[size], &buff->mem[write_head]);
            write_head += size;
            if(set_len)
            {
                buff->length = write_head - buff->start;
                buff->length -= buff->length % 2;
            }
        }

        void StereoWrite(int16_t l, int16_t r, bool rev, bool set_len)
        {
            if(!rev)
            {
                const size_t new_write_head = write_head + 2;
                if(new_write_head <= kMaxRamBuffSize)
                {
                    if(buff->undo && buff->undo->taking)
                        SaveForUndo(write_head);
                    buff->mem[write_head] = l;
                    buff->mem[write_head + 1] = r;
                    write_head = new_write_head;
                    if(set_len)
                        buff->length = write_head - buff->start;
                }
            }
            else if(write_head >= buff->start + 2)
            {
                if(buff->undo && buff->undo->taking)
                    SaveForUndo(write_head - 2);
                buff->mem[write_head - 2] = l;
                buff->mem[write_head - 1] = r;
                write_head -= 2;
            }
        }

        inline size_t GetSize() { return buff->length; }
        inline void Trim(size_t len) { buff->length = std::min(len & ~size_t(1), buff->length); }
        inline size_t GetStart() const { return buff->start; }
        inline size_t End() const { return buff->start + buff->length; }

        inline size_t GetReadHead() const { return read_head; }
        inline bool ReadEOF() { return read_head >= End() || read_head >= kMaxRamBuffSize; }
        inline bool ReadLoop(bool rev) { return (rev && read_head <= buff->start) || (!rev && ReadEOF()); }

        void SetReadHead(size_t pos) 
        { 
            if(pos <= kMaxRamBuffSize && pos <= LoopEnd())
                read_head = pos;
        }
        
        size_t GetRemainingRead(bool rev)
        {
            if(!rev && !ReadEOF())
                return End() - read_head;
            else if(rev)
                return read_head - buff->start;

            return 0;
        }

        inline size_t GetReadFromStart(bool rev) { return rev ? End() - read_head : read_head - buff->start; }

        inline size_t GetWriteHead() { return write_head; }
        inline bool WriteEOF() { return write_head >= End() || write_head >= kMaxRamBuffSize; }
        inline bool WriteFullLength() { return write_head >= kMaxRamBuffSize; }
        inline bool WriteLoop(bool rev) { return (rev && write_head <= buff->start) || (!rev && WriteEOF()); }

        void SetWriteHead(size_t pos) 
        {
            if(pos <= kMaxRamBuffSize && pos <= LoopEnd())
                write_head = pos;
        }

        size_t GetRemainingWrite(bool rev)
        {
            if(!rev && !WriteEOF())
                return End() - write_head;
            else if(rev)
                return write_head - buff->start;

            return 0;            
        }

        void AdvanceWrite(bool rev)
        {
            if(rev && write_head >= buff->start + 2)
                write_head -= 2;
            else if(!rev && write_head + 2 <= End() && write_head + 2 < kMaxRamBuffSize)
                write_head += 2;
        }

        inline bool IsCopying() const { return buff->copy_lo != buff->copy_hi; }

        inline bool CanRestore() const { return buff->halve_count; }

        void MakeLengthBase()
        {
            buff->halve_count = 0;
            buff->length_exp = 0;
            buff->base = buff->start;
        }
        inline void ClearLengthChanges()
        {
            DropUndo();
            if(buff->pending_len)
            {
                buff->start = buff->pending_start;
                buff->length = buff->pending_len;
            }
            buff->pending_len = 0;
            buff->pending = RamBufferMemory::Pending::kLength;
            buff->loading = false;
            MakeLengthBase();
        }

        // load next to the playing loop without touching it
        inline void StartLoad() { ClearLengthChanges(); buff->loading = true; }
        inline bool IsLoading() const { return buff->loading; }

        // the loaded loop takes over at the loop point
        void FinishLoad(size_t start, size_t len)
        {
            len &= ~size_t(1);
            if(len)
            {
                SetRecorded(start, len);
                buff->pending_start = start;
                buff->pending = RamBufferMemory::Pending::kRestart;
                __DMB();
                buff->pending_len = len;
            }
            buff->loading = false;
        }

        bool ApplyPendingLength(bool wrap = false)
        {
            if(!buff->pending_len)
                return false;
            __DMB(); // FinishLoad() sets the kind first
            const RamBufferMemory::Pending kind = buff->pending;
            if(kind == RamBufferMemory::Pending::kRestart && !wrap)
                return false;
            if(kind == RamBufferMemory::Pending::kCut)
                DropUndo();

            read_head = Into(kind, read_head);
            write_head = Into(kind, write_head);
            buff->start = buff->pending_start;
            buff->length = buff->pending_len;
            buff->pending_len = 0;
            if(kind != RamBufferMemory::Pending::kLength)
                MakeLengthBase();
            buff->pending = RamBufferMemory::Pending::kLength;
            return true;
        }

        // Makes the loop what the cut says, at the next loop fade.
        void SetCut()
        {
            buff->pending_start = buff->cut_start;
            buff->pending = RamBufferMemory::Pending::kCut;
            __DMB();
            buff->pending_len = (buff->cut_end - buff->cut_start) & ~size_t(1);
        }

        inline bool IsPending() const { return buff->pending_len; }
        inline int8_t LengthExp() const { return buff->length_exp; }

        inline size_t LoopLength() const { return buff->pending_len ? buff->pending_len : buff->length; }
        inline size_t LoopStart() const { return buff->pending_len ? buff->pending_start : buff->start; }

        bool CanChangeLength(int8_t steps) const
        {
            const size_t len = buff->length;
            if(IsCopying() || !steps || buff->pending_len || buff->loading)
                return false;
            if(steps > 0)
                return buff->halve_count >= steps
                       || (steps == 1 && !CanRestore() && len >= kMinLoopSize && End() + len <= kMaxRamBuffSize);
            return (len >> -steps) >= kMinLoopSize && buff->halve_count - steps <= int(sizeof(buff->halve_len) / sizeof(size_t));
        }

        inline size_t Section() const { return buff->length ? (buff->start - buff->base) / buff->length : 0; }

        bool Step(int8_t dir)
        {
            if(IsCopying() || buff->pending_len || buff->loading || buff->length_exp >= 0)
                return false;

            // stops at the first and last section
            const size_t next = Section() + dir;
            if(next >= size_t(1) << -buff->length_exp)
                return false;
            buff->pending_start = buff->base + next * buff->length;
            buff->pending_len = buff->length;
            return true;
        }

        // steps > 0 doubles, < 0 halves
        bool ChangeLength(int8_t steps, bool rev) __attribute__((optimize("Os")))
        {
            if(!CanChangeLength(steps))
                return false;

            size_t len = buff->length;
            if(steps > 0 && CanRestore())
            {
                buff->halve_count -= steps;
                len = buff->halve_len[buff->halve_count];
                // halving rounds down, so go by the section's middle
                const size_t mid = buff->start - buff->base + buff->length / 2;
                buff->start = buff->base + mid - mid % len;
                SetLoopLength(len, rev);
            }
            else if(steps > 0)
            {
                // may overwrite the undo audio
                DropUndo();

                FadeSeam(&buff->mem[End() - 2], -2);

                buff->copy_lo = 0;
                buff->copy_hi = len;
                buff->length = len * 2;
                buff->rec_len = std::max(buff->rec_len, End() - buff->rec_start);
            }
            else
            {
                for(int8_t i = steps; i < 0; i++)
                {
                    buff->halve_len[buff->halve_count++] = len;
                    len = (len / 2) & ~size_t(1);
                }
                SetLoopLength(len, rev);
            }
            buff->length_exp += steps;
            return true;
        }

        void CopyStep()
        {
            size_t lo = buff->copy_lo, hi = buff->copy_hi;
            const size_t len = buff->length / 2;
            int16_t* const mem = &buff->mem[buff->start];

            size_t n = std::min(kCopyStepSize, hi - lo);
            memcpy(&mem[lo + len], &mem[lo], n * sizeof(int16_t));
            if(lo == 0)
                FadeSeam(&mem[len], 2);
            lo += n;

            n = std::min(kCopyStepSize, hi - lo);
            hi -= n;
            memcpy(&mem[hi + len], &mem[hi], n * sizeof(int16_t));

            buff->copy_lo = lo;
            buff->copy_hi = hi;
        }

        // Audio from the other side of the loop point: samples [k, k + n) of a run of total at from are
        // added into the loop at to, faded in at the run's start or, with fade_out, out at its end.
        void Overlay(size_t from, size_t to, size_t k, size_t n, size_t total, bool fade_out)
            __attribute__((noinline, optimize("Os")))
        {
            if(from + total > kMaxRamBuffSize || to + total > kMaxRamBuffSize)
                return;

            const size_t ramp = std::min(kLoopFadeSize, total);
            for(size_t i = k; i < k + n; i += 2)
            {
                const size_t edge = fade_out ? total - 2 - i : i;
                const float gain = edge < ramp ? edge / static_cast<float>(ramp) : 1.f;
                for(size_t c = 0; c < 2; c++)
                    buff->mem[to + i + c]
                        = f2s16(s162f(buff->mem[to + i + c]) + s162f(buff->mem[from + i + c]) * gain);
            }
        }

        void LoopFadeAudio(int32_t* l, int32_t* r, bool rev) const
        {
            *l = *r = 0;
            if((rev && write_head < buff->start + 2) || (!rev && write_head + 2 > End()))
                return;

            const size_t pos = rev ? write_head - 2 : write_head;
            const size_t from_start = rev ? End() - pos : pos - buff->start;
            const size_t to_end = rev ? pos - buff->start : End() - pos;
            if(from_start >= kLoopFadeSize && to_end >= kLoopFadeSize)
                return;

            const float step = kLoopFadeStep * .5f;
            const float fade_in = from_start * step;
            const float fade_out = 1.f - (kLoopFadeSize - std::min(to_end, kLoopFadeSize)) * step;
            const float keep = 1.f - daisysp::fclamp(std::min(fade_in, fade_out), 0.f, 1.f);
            *l = buff->mem[pos] * keep;
            *r = buff->mem[pos + 1] * keep;
        }

        // idle, the input goes round the ring past the buffer
        void RollWrite(int16_t l, int16_t r)
        {
            const size_t head = roll_written % kRecRing;
            buff->mem[kMaxRamBuffSize + head] = l;
            buff->mem[kMaxRamBuffSize + head + 1] = r;
            roll_written += 2;
        }

        size_t RollWritten() const { return roll_written; } // samples, since the last RollIn
        size_t RollSize() const { return std::min(roll_written, kRecRing); }

        // the ring's last n samples go in front of the recording, which started at kRecRing
        void RollIn(size_t n)
        {
            const int16_t* ring = &buff->mem[kMaxRamBuffSize];
            int16_t* dst = &buff->mem[kRecRing - n];
            const size_t head = roll_written % kRecRing, tail = std::min(n, head); // the newest, below the head
            std::copy(ring + kRecRing - (n - tail), ring + kRecRing, dst);
            std::copy(ring + head - tail, ring + head, dst + n - tail);
            buff->start = buff->base = kRecRing - n;
            buff->length += n;
            roll_written = 0;
        }

        void ShiftWriteHead(int32_t offset)
        {
            const int32_t len = buff->length;
            if(len == 0)
                return;

            int32_t pos = (static_cast<int32_t>(write_head - buff->start) + offset) % len;
            write_head = buff->start + ((pos < 0 ? pos + len : pos) & ~1);
        }

        void StartUndoTake()
        {
            // may overwrite the loaded loop
            if(!buff->undo || buff->loading || buff->pending == RamBufferMemory::Pending::kRestart)
                return;
            UndoMemory& u = *buff->undo;

            DropUndo();
            const size_t len = CanRestore() ? buff->halve_len[0] : buff->length;
            const size_t chunks = (len + kUndoChunk - 1) / kUndoChunk;
            const size_t store = kMaxRamBuffSize - chunks * kUndoChunk;
            if(buff->base + chunks * kUndoChunk > store)
                return;

            std::fill(u.saved, u.saved + (chunks + 31) / 32, 0);
            u.lo = buff->base;
            u.store = store;
            u.chunks = u.next = chunks;
            u.taking = true;
        }

        inline void DropUndo()
        {
            if(!buff->undo)
                return;
            UndoMemory& u = *buff->undo;
            u.chunks = u.next = 0;
            u.taking = u.pending = false;
        }

        inline bool HasUndo() const { return buff->undo->chunks; }
        inline bool IsUndoing() const { return buff->undo->pending || buff->undo->next < buff->undo->chunks; }

        bool RequestUndo()
        {
            if(!HasUndo() || IsUndoing() || IsCopying())
                return false;
            buff->undo->pending = true;
            return true;
        }

        bool StartUndo(bool rev)
        {
            UndoMemory& u = *buff->undo;
            if(!u.pending)
                return false;
            u.pending = u.taking = false;

            const size_t lo = u.lo, end = lo + u.chunks * kUndoChunk;
            u.loop_lo = u.loop_n = 0;
            if(buff->start >= lo && End() <= end && buff->length)
            {
                u.loop_lo = (buff->start - lo) / kUndoChunk;
                u.loop_n = (End() - 1 - lo) / kUndoChunk + 1 - u.loop_lo;
            }
            const size_t head = std::min(std::max(read_head, buff->start), End() - 1);
            u.from = head >= lo ? (head - lo) / kUndoChunk - u.loop_lo : 0;
            u.rev = rev;
            u.next = 0;
            UndoStep();
            UndoStep();
            return true;
        }

        void UndoStep()
        {
            UndoMemory& u = *buff->undo;
            for(size_t scan = 0; scan < kUndoScan && u.next < u.chunks; scan++)
            {
                const size_t c = UndoChunkAt(u.next++);
                if(u.saved[c / 32] & (1u << (c % 32)))
                {
                    int16_t* const loop = &buff->mem[u.lo + c * kUndoChunk];
                    std::swap_ranges(loop, loop + kUndoChunk, &buff->mem[u.store + c * kUndoChunk]);
                    return;
                }
            }
        }

        void FadeJoin(size_t pos, size_t end)
        {
            if(pos < buff->start + kLoopFadeSize || pos + kLoopFadeSize > end)
                return;
            FadeSeam(&buff->mem[pos - 2], -2);
            FadeSeam(&buff->mem[pos], 2);
        }

    private:
        inline size_t LoopEnd() const { return LoopStart() + LoopLength(); }

        void SetRecorded(size_t start, size_t len)
        {
            buff->rec_start = buff->cut_start = start;
            buff->rec_len = len;
            buff->cut_end = start + len;
        }

        void SetLoopLength(size_t len, bool rev)
        {
            if(len >= buff->length || read_head - buff->start + (rev ? 0 : kLoopFadeSize) < len)
            {
                buff->length = len;
                buff->pending_len = 0;
            }
            else
            {
                buff->pending_start = buff->start;
                buff->pending_len = len;
            }
        }

        void SaveForUndo(size_t pos) __attribute__((noinline))
        {
            UndoMemory& u = *buff->undo;
            const size_t c = (pos - u.lo) / kUndoChunk; // wraps if before lo
            if(c >= u.chunks)
            {
                DropUndo();
                return;
            }
            uint32_t& word = u.saved[c / 32];
            const uint32_t bit = 1u << (c % 32);
            if(word & bit)
                return;
            word |= bit;
            memcpy(&buff->mem[u.store + c * kUndoChunk], &buff->mem[u.lo + c * kUndoChunk],
                   kUndoChunk * sizeof(int16_t));
        }

        size_t UndoChunkAt(size_t i) const
        {
            UndoMemory& u = *buff->undo;
            const size_t lo = u.loop_lo, n = u.loop_n;
            if(i < n)
                return lo + (u.rev ? u.from + n - i : u.from + i) % n;
            i -= n;
            return i < lo ? i : i + n;
        }

        // Returns where a head is once the pending bounds apply.
        size_t Into(RamBufferMemory::Pending kind, size_t head) const
        {
            const size_t start = buff->pending_start, end = start + buff->pending_len;
            if(kind == RamBufferMemory::Pending::kRestart)
                return start;
            if(kind == RamBufferMemory::Pending::kCut && head >= start && head <= end)
                return head;
            const size_t rel = head > buff->start ? head - buff->start : 0;
            return start + rel % buff->pending_len;
        }

        static void FadeSeam(int16_t* seam, int step) __attribute__((noinline, optimize("Os")))
        {
            for(size_t i = 0; i < kLoopFadeSize; i += 2, seam += step)
            {
                const float gain = i / static_cast<float>(kLoopFadeSize);
                seam[0] *= gain;
                seam[1] *= gain;
            }
        }

        RamBufferMemory* buff;
        size_t roll_written = 0;
        size_t read_head, write_head;
    };
} // namespace daisy