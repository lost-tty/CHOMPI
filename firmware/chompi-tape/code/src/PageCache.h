#pragma once

#include <stdint.h>
#include <algorithm>
#include "daisy.h"

namespace chompi
{
    // 64 KB blocks in SDRAM, each holding a page of an owner, in a list from the free ones and the one used
    // longest ago to the one used last. Only the card side takes and frees blocks; who reads them looks them up
    // again when the epoch changes.
    class PageCache
    {
      public:
        static constexpr uint32_t kBytes = 65536;
        static constexpr int kMaxBlocks = 1024;
        static constexpr int kOwners = 32; // owner ids stay below it
        static constexpr uint8_t kFree = 0xff;

        struct Block
        {
            uint8_t owner = kFree;
            uint16_t page = 0;
            volatile uint8_t seq = 0;  // which content of its owner it holds
            volatile uint16_t gen = 0; // bumped by whoever writes it
        };

        // how Victim treats a block: take it, take it only when nothing else goes, keep it and count it as used,
        // pass it
        enum Choice : uint8_t { kTake, kLast, kKeep, kSkip };

        // mem: count blocks, 32-byte aligned, in SDRAM
        void Init(uint8_t *mem, int count)
        {
            mem_ = mem;
            count_ = std::min(count, kMaxBlocks);
            head_ = tail_ = -1;
            for(int b = 0; b < count_; b++)
            {
                blocks_[b] = Block();
                Append(b);
            }
            free_ = count_;
            epoch = 1;
        }

        uint8_t *Data(int b) const { return mem_ + size_t(b) * kBytes; }
        Block &operator[](int b) { return blocks_[b]; }
        const Block &operator[](int b) const { return blocks_[b]; }
        int Count() const { return count_; }
        int Free() const { return free_; }

        // b holds owner's page from now on; its owner forgot what it held
        void Claim(int b, uint8_t owner, uint16_t page, uint8_t seq)
        {
            Release(b);
            free_--;
            blocks_[b] = {owner, page, seq, 0};
            Use(b);
        }

        void Use(int b)
        {
            if(blocks_[b].owner == kFree || b == tail_)
                return;
            Unlink(b);
            Append(b);
        }

        // what b holds goes; its owner forgot it
        void Release(int b)
        {
            if(blocks_[b].owner == kFree)
                return;
            epoch = epoch + 1;
            __DMB();
            blocks_[b] = Block();
            free_++;
            Unlink(b);
            Prepend(b);
        }

        // A free block, else the first from the front that choose lets go and clean says needs no write; a kLast
        // one only when no other is. -1 for none, dirty then the first block to write out, or -1.
        template <class Choose, class Clean>
        int Victim(Choose choose, Clean clean, int &dirty)
        {
            dirty = -1;
            if(free_)
                return head_;
            int last = -1;
            for(int b = head_, n = count_; b >= 0 && n > 0; n--)
            {
                const int next = next_[b];
                const Choice c = choose(b);
                if(c == kKeep)
                    Use(b);
                else if(c != kSkip && !clean(b))
                    dirty = dirty < 0 ? b : dirty;
                else if(c == kTake)
                {
                    dirty = -1;
                    return b;
                }
                else if(c == kLast && last < 0)
                    last = b;
                b = next;
            }
            if(last >= 0)
                dirty = -1;
            return last;
        }

        template <class F>
        void Walk(F f) const
        {
            for(int b = head_; b >= 0 && f(b); b = next_[b])
            {
            }
        }

        volatile uint32_t epoch;

      private:
        void Append(int b)
        {
            prev_[b] = int16_t(tail_);
            next_[b] = -1;
            (tail_ >= 0 ? next_[tail_] : head_) = int16_t(b);
            tail_ = b;
        }
        void Prepend(int b)
        {
            next_[b] = int16_t(head_);
            prev_[b] = -1;
            (head_ >= 0 ? prev_[head_] : tail_) = int16_t(b);
            head_ = b;
        }
        void Unlink(int b)
        {
            (prev_[b] >= 0 ? next_[prev_[b]] : head_) = next_[b];
            (next_[b] >= 0 ? prev_[next_[b]] : tail_) = prev_[b];
        }

        uint8_t *mem_;
        int count_, free_;
        Block blocks_[kMaxBlocks];
        int16_t prev_[kMaxBlocks], next_[kMaxBlocks], head_, tail_;
    };
}
