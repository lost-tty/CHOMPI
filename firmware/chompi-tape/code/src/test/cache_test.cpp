// Tests the page cache and its tapes on the host: which block goes, what a lookup finds, how a file's frames read.
#include <cstdio>
#include <vector>
#include "Tape.h"

using namespace chompi;

static int failures = 0;
#define CHECK(cond)                                           \
    do                                                        \
    {                                                         \
        if(!(cond))                                           \
        {                                                     \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                       \
        }                                                     \
    } while(0)

static constexpr int kBlocks = 4;

struct Rig
{
    std::vector<uint8_t> mem = std::vector<uint8_t>(kBlocks * PageCache::kBytes);
    std::vector<uint16_t> table = std::vector<uint16_t>(Tape::kMax + 1);
    PageCache cache;
    Tape tape;

    Rig()
    {
        cache.Init(mem.data(), kBlocks);
        tape.Init(&cache, 0, table.data(), nullptr);
    }
    // takes a block for the tape's page, and lets one go, as the card side does
    int Take(uint32_t page)
    {
        int dirty;
        const int b = cache.Victim([](int) { return PageCache::kTake; }, [](int) { return true; }, dirty);
        if(b >= 0)
        {
            if(cache[b].owner != PageCache::kFree)
                tape.Unmap(cache[b].page, b);
            cache.Claim(b, 0, uint16_t(page), tape.Seq());
            tape.Map(page, b);
        }
        return b;
    }
    void Drop(int b)
    {
        tape.Unmap(cache[b].page, b);
        cache.Release(b);
    }
};

// the block used longest ago goes; one kept moves to the back, one passed stays, one taken last goes last, one not
// written out not at all
static void Victims()
{
    Rig r;
    int b[kBlocks];
    for(int p = 0; p < kBlocks; p++)
        b[p] = r.Take(p);
    CHECK(r.cache.Free() == 0);
    r.cache.Use(b[0]);
    int dirty;
    auto clean = [](int) { return true; };
    CHECK(r.cache.Victim([](int) { return PageCache::kTake; }, clean, dirty) == b[1]);
    CHECK(r.cache.Victim([&](int x) { return x == b[1] ? PageCache::kKeep : PageCache::kTake; }, clean, dirty) == b[2]);
    CHECK(r.cache.Victim([](int) { return PageCache::kTake; }, clean, dirty) == b[2]); // b[1] went to the back
    CHECK(r.cache.Victim([&](int x) { return x == b[2] ? PageCache::kSkip : PageCache::kTake; }, clean, dirty) == b[3]);
    CHECK(r.cache.Victim([](int) { return PageCache::kTake; }, clean, dirty) == b[2]); // b[2] stayed at the front
    CHECK(r.cache.Victim([](int) { return PageCache::kLast; }, clean, dirty) >= 0); // only kLast ones: one goes
    CHECK(r.cache.Victim([&](int x) { return x == b[3] ? PageCache::kTake : PageCache::kLast; }, clean, dirty) == b[3]);
    CHECK(r.cache.Victim([](int) { return PageCache::kTake; }, [](int) { return false; }, dirty) < 0);
    CHECK(dirty >= 0); // nothing goes without a write: the first to write out
}

// a block that goes leaves the tape, and a reader looks it up again
static void Lookups()
{
    Rig r;
    r.tape.InitScratch();
    const int b = r.Take(0);
    r.Take(1);
    int16_t *w = r.tape.Write(5);
    CHECK(w);
    *w = 1234;
    CHECK(r.cache[b].gen != 0); // written
    CHECK(r.tape.Read(5) && *r.tape.Read(5) == 1234);
    CHECK(r.tape.Ready(0, 2 * Tape::kSamples) && !r.tape.Ready(0, 2 * Tape::kSamples + 1));
    size_t n = Tape::kSamples;
    CHECK(r.tape.Run(Tape::kSamples - 10, n, false) && n == 10); // a run ends with its page

    r.Drop(b);
    CHECK(!r.tape.In(0));
    CHECK(!r.tape.Read(5)); // the epoch moved: the reader doesn't find the block that went
    CHECK(r.cache.Free() == kBlocks - 1);
    CHECK(r.Take(7) == b); // the free block comes first
}

// a file's frames read in its format, also across a page
static void Frames()
{
    Rig r;
    const uint32_t offset = 44 + 2; // a frame straddles the first page's end
    for(int p = 0; p < 2; p++)
        r.Take(p);
    uint8_t *const d0 = r.cache.Data(r.tape.Slot(0)), *const d1 = r.cache.Data(r.tape.Slot(1));
    const uint32_t f = (PageCache::kBytes - offset) / 4; // the frame across the page end
    const uint32_t at = offset + f * 4;
    d0[at] = 0x34, d0[at + 1] = 0x12;   // left, little-endian
    d1[at + 2 - PageCache::kBytes] = 0x78, d1[at + 3 - PageCache::kBytes] = 0x56;
    r.tape.SetFile({offset, f + 1, 2, true, 48000.f}, false);
    Tape::Tlb tlb;
    tlb.Flush();
    float l, rr;
    CHECK(r.tape.Frame(f, tlb, l, rr));
    CHECK(l == 0x1234 / 32768.f && rr == 0x5678 / 32768.f);

    r.tape.SetFile({offset, f + 1, 1, false, 48000.f}, true); // mono, big-endian
    CHECK(r.tape.Frame(f * 2, tlb, l, rr) && l == rr && l == 0x3412 / 32768.f);
}

int main()
{
    Victims();
    Lookups();
    Frames();
    printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures != 0;
}
