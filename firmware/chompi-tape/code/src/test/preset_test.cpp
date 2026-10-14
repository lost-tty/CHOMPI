// Tests presets.json on the host: what we write reads back, and what the TAPE firmware wrote still reads.
#include <cstdio>
#include <string>
#include "PresetManager.h"

using chompi::PresetManager;
using Preset = PresetManager::Preset;
using Result = PresetManager::Result;

static int failures = 0;
#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if(!(cond))                                                \
        {                                                          \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            failures++;                                            \
        }                                                          \
    } while(0)

constexpr size_t kFileSize = PresetManager::kFileSize;
constexpr int kControls = PresetManager::kMaxControls;
constexpr int kFileControls = PresetManager::kFileControls;
constexpr int kV1Controls = 7;
constexpr float kVolume = .75f;
constexpr float kEps = 1e-4f;
const float kDefaults[kControls] = {.5f, 0.f, 1.f, 0.f, 1.f, 1.f, 1.f, .8f, .5f, 0.f, 0.f};
const float kA[kFileControls] = {.25f, .1f, .9f, .2f, .3f, 1.f, 0.f, .7f, .35f};
const float kB[kFileControls] = {.75f, .05f, .95f, .4f, .6f, 0.f, 1.f, .9f, .65f};
static char file[kFileSize];

static bool Near(float a, float b)
{
    return a - b < kEps && b - a < kEps;
}

// Fills out with the first n of the values, the defaults after.
static void Fill(float *out, const float *values, int n)
{
    for(int i = 0; i < kControls; i++)
        out[i] = i < n ? values[i] : kDefaults[i];
}

static Preset Make(const float *values, int n)
{
    float f[kControls];
    Fill(f, values, n);
    return {f[PresetManager::kPitch], f[PresetManager::kStart],  f[PresetManager::kEnd],    f[PresetManager::kAttack],
            f[PresetManager::kDecay], f[PresetManager::kGain],   f[PresetManager::kPan],    f[PresetManager::kGlide],
            f[PresetManager::kAutoloop] > .5f, f[PresetManager::kSustain] > .5f, f[PresetManager::kLegato] > .5f};
}

static bool Same(const Preset &a, const Preset &b)
{
    return Near(a.pitch, b.pitch) && Near(a.start, b.start) && Near(a.end, b.end) && Near(a.attack, b.attack)
           && Near(a.decay, b.decay) && Near(a.gain, b.gain) && Near(a.pan, b.pan) && Near(a.glide, b.glide)
           && a.autoloop == b.autoloop && a.sustain == b.sustain && a.legato == b.legato;
}

// Returns whether the preset holds the first n of the values, the defaults after.
static bool Matches(const Preset &p, const float *values, int n)
{
    return Same(p, Make(values, n));
}

static void Init(PresetManager &m)
{
    m.Init(kDefaults, kVolume);
}

static Result Parse(PresetManager &m, const std::string &text)
{
    Init(m);
    snprintf(file, kFileSize, "%s", text.c_str());
    return m.Parse(file, kFileSize);
}

static bool OnlyValid(PresetManager &m, int mode_a, int bank_a, int slot_a, int mode_b, int bank_b, int slot_b)
{
    for(int mode = 0; mode < PresetManager::kMaxModes; mode++)
        for(int bank = 0; bank < PresetManager::kMaxBanks; bank++)
            for(int slot = 1; slot <= PresetManager::kMaxSlots; slot++)
            {
                const bool expected = (mode == mode_a && bank == bank_a && slot == slot_a)
                                      || (mode == mode_b && bank == bank_b && slot == slot_b);
                if(m.IsValid(mode, bank, slot) != expected)
                    return false;
            }
    return true;
}

// Builds the TAPE firmware's layout: two modes, five banks, fourteen slots of n values and a flag. Slots
// (0, a, 1) and (1, e, 14) hold kA and kB; the rest are empty.
static std::string Stock(int n, const char *tail)
{
    std::string s = "[";
    for(int mode = 0; mode < PresetManager::kMaxModes; mode++)
    {
        s += mode ? ",[" : "[";
        for(int bank = 0; bank < PresetManager::kMaxBanks; bank++)
        {
            s += bank ? ",[" : "[";
            for(int slot = 0; slot < PresetManager::kMaxSlots; slot++)
            {
                const bool a = mode == 0 && bank == 0 && slot == 0;
                const bool b = mode == 1 && bank == 4 && slot == 13;
                const float *v = a ? kA : b ? kB : kDefaults;
                s += slot ? ",[" : "[";
                for(int i = 0; i < n; i++)
                {
                    char t[32];
                    snprintf(t, sizeof(t), "%.5f,", double(v[i]) * 1000);
                    s += t;
                }
                s += a || b ? "true]" : "false]";
            }
            s += "]";
        }
        s += "]";
    }
    s += tail;
    s += "]";
    return s;
}

static void RoundTrip()
{
    PresetManager a;
    Init(a);
    // values exact in binary, so the text checks below hold
    const Preset all = {.25f, .125f, .875f, .25f, .375f, .75f, .375f, .5f, true, true, true};
    const Preset legato = {.5f, 0.f, 1.f, 0.f, 1.f, .75f, .5f, 0.f, false, false, true};
    const Preset glide = {.5f, 0.f, 1.f, 0.f, 1.f, .75f, .5f, .25f, false, false, false};
    const Preset plain = {.5f, 0.f, 1.f, 0.f, 1.f, .75f, .5f, 0.f, false, false, false};
    const Preset autoloop = {.5f, 0.f, 1.f, 0.f, 1.f, .75f, .5f, 0.f, true, false, false};
    const Preset sustain = {.5f, 0.f, 1.f, 0.f, 1.f, .75f, .5f, 0.f, false, true, false};
    a.Store(1, 2, 5, all);
    a.Store(0, 0, 1, legato);
    a.Store(0, 4, 14, glide);
    a.Store(1, 1, 7, plain);
    a.Store(0, 1, 2, autoloop);
    a.Store(0, 1, 3, sustain);
    a.SetVolume(.6f);
    // the flags land in the values array as 0 or 1, which the writer reads
    CHECK(a.Values(1, 2, 5)[PresetManager::kAutoloop] == 1.f && a.Values(1, 2, 5)[PresetManager::kSustain] == 1.f
          && a.Values(1, 2, 5)[PresetManager::kLegato] == 1.f);
    CHECK(a.Values(0, 0, 1)[PresetManager::kAutoloop] == 0.f && a.Values(0, 0, 1)[PresetManager::kSustain] == 0.f
          && a.Values(0, 0, 1)[PresetManager::kLegato] == 1.f);
    CHECK(a.WriteWholeFile(file, kFileSize) == Result::OK);
    const std::string written = file;
    // the slot's array keeps the TAPE order: pitch, start, end, attack, decay, autoloop, sustain, gain, pan
    CHECK(written.find("[250.00000,125.00000,875.00000,250.00000,375.00000,1000.00000,1000.00000,750.00000,"
                       "375.00000,true]")
          != std::string::npos);
    CHECK(written.find("[500.00000,0.00000,1000.00000,0.00000,1000.00000,1000.00000,0.00000,750.00000,500.00000,true]")
          != std::string::npos);
    CHECK(written.find("[500.00000,0.00000,1000.00000,0.00000,1000.00000,0.00000,1000.00000,750.00000,500.00000,true]")
          != std::string::npos);
    CHECK(written.find("\"slots\":{\"jammi-a-1\":{\"legato\":1,\"glide\":0.000},"
                       "\"jammi-e-14\":{\"legato\":0,\"glide\":0.250},"
                       "\"cubbi-c-5\":{\"legato\":1,\"glide\":0.500}}")
          != std::string::npos);

    PresetManager b;
    Init(b);
    CHECK(b.Parse(file, kFileSize) == Result::OK);
    CHECK(Same(b.Recall(1, 2, 5), all));
    CHECK(Same(b.Recall(0, 0, 1), legato));
    CHECK(Same(b.Recall(0, 4, 14), glide));
    CHECK(Same(b.Recall(1, 1, 7), plain));
    CHECK(Same(b.Recall(0, 1, 2), autoloop));
    CHECK(Same(b.Recall(0, 1, 3), sustain));
    CHECK(b.Recall(0, 1, 2).autoloop && !b.Recall(0, 1, 2).sustain && !b.Recall(0, 1, 2).legato);
    CHECK(!b.Recall(0, 1, 3).autoloop && b.Recall(0, 1, 3).sustain && !b.Recall(0, 1, 3).legato);
    CHECK(b.IsValid(1, 2, 5) && b.IsValid(0, 0, 1) && b.IsValid(0, 4, 14) && b.IsValid(1, 1, 7));
    CHECK(!b.IsValid(0, 0, 2));
    CHECK(Matches(b.Recall(0, 0, 2), kDefaults, kControls));
    CHECK(Near(b.Volume(), .6f));
    CHECK(written == file); // Parse writes the file again: the same text
}

static void StockV2()
{
    PresetManager m;
    CHECK(Parse(m, Stock(kFileControls, ",2,{\"volume\":500.00000}")) == Result::OK);
    CHECK(OnlyValid(m, 0, 0, 1, 1, 4, 14));
    CHECK(Matches(m.Recall(0, 0, 1), kA, kFileControls));
    CHECK(Matches(m.Recall(1, 4, 14), kB, kFileControls));
    CHECK(Near(m.Volume(), .5f));
}

static void StockV2Spaced()
{
    std::string text = Stock(kFileControls, ",2,{\"volume\":500.00000}");
    std::string spaced;
    for(char c : text)
    {
        spaced += c;
        if(c == ',' || c == '[' || c == ':')
            spaced += "\n  ";
    }
    PresetManager m;
    CHECK(Parse(m, spaced) == Result::OK);
    CHECK(OnlyValid(m, 0, 0, 1, 1, 4, 14));
    CHECK(Matches(m.Recall(0, 0, 1), kA, kFileControls));
    CHECK(Matches(m.Recall(1, 4, 14), kB, kFileControls));
    CHECK(Near(m.Volume(), .5f));
}

static void StockV1()
{
    PresetManager m;
    CHECK(Parse(m, Stock(kV1Controls, "")) == Result::OK);
    CHECK(OnlyValid(m, 0, 0, 1, 1, 4, 14));
    CHECK(Matches(m.Recall(0, 0, 1), kA, kV1Controls));
    CHECK(Matches(m.Recall(1, 4, 14), kB, kV1Controls));
    CHECK(Near(m.Volume(), kVolume));
}

static void Slots()
{
    const char *tail = ",2,{\"volume\":250.00000,\"slots\":{"
                       "\"jammi-a-1\":{\"legato\":1,\"glide\":0.250},"
                       "\"cubbi-b-3\":{\"legato\":1,\"glide\":0.5},"
                       "\"cubbi-e-14\":{\"legato\":0,\"glide\":2.0}}}";
    PresetManager m;
    CHECK(Parse(m, Stock(kFileControls, tail)) == Result::OK);
    CHECK(OnlyValid(m, 0, 0, 1, 1, 4, 14));
    float a[kControls], b[kControls];
    Fill(a, kA, kFileControls);
    Fill(b, kB, kFileControls);
    a[PresetManager::kLegato] = 1.f;
    a[PresetManager::kGlide] = .25f;
    b[PresetManager::kLegato] = 0.f;
    b[PresetManager::kGlide] = 1.f; // clamped
    CHECK(Matches(m.Recall(0, 0, 1), a, kControls));
    CHECK(Matches(m.Recall(1, 4, 14), b, kControls));
    CHECK(Matches(m.Recall(1, 1, 3), kDefaults, kControls)); // settings for an empty slot change nothing
    CHECK(Near(m.Volume(), .25f));
}

static void Broken()
{
    PresetManager m;
    CHECK(Parse(m, "[[[[") == Result::ERR_INVALID_JSON);
    CHECK(OnlyValid(m, -1, -1, -1, -1, -1, -1));
    CHECK(Near(m.Volume(), kVolume));
    CHECK(std::string(file).rfind("[[[[", 0) == 0); // written again, from the defaults
}

int main()
{
    RoundTrip();
    StockV2();
    StockV2Spaced();
    StockV1();
    Slots();
    Broken();
    if(failures)
    {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
