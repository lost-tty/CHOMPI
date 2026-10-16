#pragma once

#include "Stream.h"
#include "SdDma.h"

namespace chompi
{
    // Fills the voices' rings from the card in the card task, the one that runs dry soonest first.
    // The card is read in blocks into a cache the rings share: voices on the same file read it once.
    class Streamer
    {
      public:
        static constexpr uint32_t kBlockFrames = 16384;
        static constexpr uint32_t kBlockBytes = kBlockFrames * 4 + 512; // a block, and the part of its first sector before it

        // files, head, rings and cache: in SDRAM; head 512 bytes, cache blocks * kBlockBytes, both 32-byte aligned;
        // rings count * 2 * Ring::kFrames frames
        void Init(Stream *streams, int count, SdFile *files, uint8_t *head, int16_t *rings, uint8_t *cache, int blocks);

        // the card task, while the card is free: opens and restarts, fills the rings it can from the cache,
        // and starts one transfer if a ring needs a block. true when it started one.
        bool Step();

        // files were written: the cache may hold their old content
        void Forget() { forget_ = true; }

      private:
        static constexpr int kMaxStreams = 8;
        static constexpr int kMaxBlocks = 32;

        struct State
        {
            uint32_t restart_seq;
            uint32_t file_seq; // open_seq of the file it was restarted for
            uint32_t cursor;   // the next file frame to fill
            bool ended;
        };

        struct Block
        {
            uint32_t file, index; // index: of the file's blocks
            uint32_t used;        // when it was last read, for which goes first
            uint32_t skip;        // bytes before the block's first frame
            bool valid;
        };

        void Open(int i);
        void Restart(int i, int r);
        int Neediest(uint32_t skip) const;
        void Fill(int i, int r, bool may_read);
        bool Track(int i, int r, const Span &span);
        bool Repeat(Ring &g, State &st, const Span &span, uint32_t room);
        int Cached(uint32_t file, uint32_t index) const;
        int Victim() const;
        void Copy(int i, int r, int b, uint32_t first, uint32_t n);
        void Commit();
        bool AddRun(Ring &g, uint32_t at, uint32_t pos, const Span &span, bool jump);
        Span SpanOf(int i, int r) const;

        Stream *streams_;
        SdFile *files_;
        uint8_t *head_;
        int count_;
        uint32_t open_seq_[kMaxStreams];
        uint32_t data_at_[kMaxStreams]; // bytes before the frames
        uint32_t file_[kMaxStreams];    // which file, the same for the same file
        State state_[kMaxStreams][2];

        uint8_t *cache_;
        Block blocks_[kMaxBlocks];
        int block_count_;
        uint32_t clock_;
        volatile bool forget_;

        int reading_; // the block under way, -1 for none
        int reading_stream_;
    };

    extern Streamer streamer;
}
