// What the page cache, the looper and the voices use of libDaisy, on the host.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <cmath>
#define __DMB() ((void)0)
inline float s162f(int16_t x) { return x * (1.f / 32768.f); }
inline int16_t f2s16(float x) { return int16_t(std::max(-1.f, std::min(1.f, x)) * 32767.f); }
namespace daisy
{
    struct System
    {
        static uint32_t now;
        static uint32_t GetNow() { return now; }
        static uint32_t GetTick() { return now * 1000; } // a tick per µs
        static uint32_t GetTickFreq() { return 1000000; }
    };

    // the libDaisy FIFO as Sampler.h uses it
    template <typename T, size_t kCapacity>
    class FIFO
    {
      public:
        void Clear() { head_ = n_ = 0; }
        bool PushBack(const T &x)
        {
            if(n_ == kCapacity)
                return false;
            buf_[(head_ + n_++) % kCapacity] = x;
            return true;
        }
        T PopFront()
        {
            const T x = buf_[head_];
            head_ = (head_ + 1) % kCapacity;
            n_--;
            return x;
        }
        size_t GetNumElements() const { return n_; }

      private:
        T buf_[kCapacity];
        size_t head_ = 0, n_ = 0;
    };
}
