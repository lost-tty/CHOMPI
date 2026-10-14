#pragma once

#include <stdint.h>
#include <algorithm>

namespace chompi
{
    // Keeps the keys down in the order pressed.
    class Keys
    {
      public:
        struct Key
        {
            int key;
            float nn, vel;
        };
        static constexpr int kMax = 16;

        void Clear() { count_ = 0; }

        void Press(int key, float nn, float vel)
        {
            Release(key);
            if(count_ == kMax)
                Remove(0);
            keys_[count_++] = {key, nn, vel};
        }

        bool Release(int key)
        {
            const int i = Find(key);
            if(i < 0)
                return false;
            Remove(i);
            return true;
        }

        int Count() const { return count_; }

        const Key &Last() const { return keys_[count_ - 1]; }

      private:
        int Find(int key) const
        {
            for(int i = 0; i < count_; i++)
                if(keys_[i].key == key)
                    return i;
            return -1;
        }

        void Remove(int i)
        {
            std::copy(keys_ + i + 1, keys_ + count_, keys_ + i);
            count_--;
        }

        Key keys_[kMax];
        int count_ = 0;
    };
}
