#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include "FileStreamingManager.h"
#include "core_json.h"
#include "Banks.h"

#define PRE_VERSION "2"

namespace chompi {
class PresetManager
{
    public:
    PresetManager() {}
    ~PresetManager() {}

    void Init(const float* defaults, float volume_default)
    {
        updated = true;
        defaults_ = defaults;

        // fill in defaults. probably a more efficient way to do this with memcpy
        for(size_t mode = 0; mode < kMaxModes; mode++)
        {
            for(size_t bank = 0; bank < kMaxBanks; bank++)
            {
                for(size_t slot = 0; slot < kMaxSlots; slot++)
                {
                    for(int control = 0; control < kMaxControls; control++)
                    {
                        if(slot == 14)
                            chompi_value[control] = defaults[control];
                        else
                            values[mode][bank][slot][control] = defaults[control];
                    }
                    values_valid[mode][bank][slot] = false;
                }
            }
        }
        chompi_valid = false;
        volume = volume_default;
    }

    // The main volume is saved with the presets.
    float Volume() const { return volume; }

    void SetVolume(float v)
    {
        volume = v;
        updated = true;
    }

    static constexpr size_t kFileSize = 24576; // bytes
    static constexpr int kMaxModes = 2; // jammi, cubbi
    static constexpr int kMaxBanks = kNumBanks;
    static constexpr int kMaxSlots = 14; // 1, 2, 3, etc.
    static constexpr int kMaxControls = 11;
    // The first nine are the slot's array in the file, as the TAPE firmware reads it.
    static constexpr int kFileControls = 9;
    // A slot's values in the file's order; legato and glide sit in the settings object, under the slot's name.
    enum Control { kPitch, kStart, kEnd, kAttack, kDecay, kAutoloop, kSustain, kGain, kPan, kLegato, kGlide };
    // Holds one slot's settings.
    struct Preset
    {
        float pitch, start, end, attack, decay, gain, pan, glide;
        bool autoloop, sustain, legato;
    };
    // Returns the slot's preset, or the defaults for a slot without one.
    Preset Recall(size_t mode, size_t bank, size_t slot)
    {
        const float *v = IsValid(mode, bank, slot) ? Values(mode, bank, slot) : defaults_;
        return {v[kPitch], v[kStart], v[kEnd], v[kAttack], v[kDecay], v[kGain], v[kPan], v[kGlide],
                v[kAutoloop] > .5f, v[kSustain] > .5f, v[kLegato] > .5f};
    }

    void Store(size_t mode, size_t bank, size_t slot, const Preset &p)
    {
        if(slot < 1 || slot > kMaxSlots + 1 || mode >= kMaxModes || bank >= kMaxBanks)
            return;
        float *v = Values(mode, bank, slot);
        v[kPitch] = p.pitch;
        v[kStart] = p.start;
        v[kEnd] = p.end;
        v[kAttack] = p.attack;
        v[kDecay] = p.decay;
        v[kGain] = p.gain;
        v[kPan] = p.pan;
        v[kGlide] = p.glide;
        v[kAutoloop] = float(p.autoloop);
        v[kSustain] = float(p.sustain);
        v[kLegato] = float(p.legato);
        if(slot == 15)
            chompi_valid = true;
        else
        {
            values_valid[mode][bank][slot - 1] = true;
            updated = true;
        }
    }

    enum class Result
    {
        OK,
        ERR_BUFF_OVERFLOW,
        ERR_INVALID_JSON,
        ERR_GENERIC,
    };

    Result WriteWholeFile(char* buffer, size_t size)
    {
        if(!updated)
            return Result::ERR_GENERIC;

        updated = false;

        std::fill_n(buffer, size, '\0');
        strcpy(buffer, "[");
        written_ = 1;
        char append[64];
        for(int mode = 0; mode < kMaxModes; mode++)
        {
            sprintf(append,"[");
            StrAppend(buffer, append);

            for(int bank = 0; bank < kMaxBanks; bank++)
            {
                sprintf(append,"[");
                StrAppend(buffer, append);

                for(int slot = 0; slot < kMaxSlots; slot++)
                {
                    sprintf(append,"[");
                    StrAppend(buffer, append);

                    for(int ctrl = 0; ctrl < kFileControls; ctrl++)
                    {
                        sprintf(append, "%.5f,", double(values[mode][bank][slot][ctrl]) * 1000); // as fine as the float
                        StrAppend(buffer, append);
                    }


                    char valid[6];
                    if(values_valid[mode][bank][slot])
                        strcpy(valid, "true");
                    else
                        strcpy(valid, "false");

                    sprintf(append,"%s],", valid);
                    StrAppend(buffer, append);
                }

                buffer[--written_] = '\0';
                sprintf(append,"],");
                StrAppend(buffer, append);
            }

            buffer[--written_] = '\0';
            sprintf(append,"],");
            StrAppend(buffer, append);
        }
        // buffer[strlen(buffer) - 1] = '\0';

        strcpy(append, PRE_VERSION);
        StrAppend(buffer, append);
        sprintf(append, ",{\"volume\":%.5f,\"slots\":{", double(volume) * 1000);
        StrAppend(buffer, append);
        bool first = true;
        for(int mode = 0; mode < kMaxModes; mode++)
            for(int bank = 0; bank < kMaxBanks; bank++)
                for(int slot = 0; slot < kMaxSlots; slot++)
                {
                    const float *v = values[mode][bank][slot];
                    if(!values_valid[mode][bank][slot] || (v[kLegato] < .5f && v[kGlide] <= 0.f))
                        continue;
                    sprintf(append, "%s\"%s-%c-%d\":{\"legato\":%d,\"glide\":%.3f}", first ? "" : ",", ModeName(mode),
                            kBanks[bank].letter, slot + 1, v[kLegato] > .5f, double(v[kGlide]));
                    StrAppend(buffer, append);
                    first = false;
                }
        strcpy(append, "}}");
        StrAppend(buffer, append);
        strcpy(append, "]"),
        StrAppend(buffer, append);

        return Result::OK;
    }

    // Walks the file once. A V1 file has no version and seven values a slot.
    Result Parse(char* buffer, size_t size)
    {
        updated = true;
        Result ret = Result::ERR_INVALID_JSON;
        const size_t len = strlen(buffer);
        if(JSON_Validate(buffer, len) == JSONSuccess)
        {
            std::fill_n(&values_valid[0][0][0], kMaxModes * kMaxBanks * kMaxSlots, false);
            char *value;
            size_t value_len;
            const int numcontrols = JSON_Search(buffer, len, "[2]", 3, &value, &value_len) == JSONSuccess ? kFileControls : 7;
            Items root(buffer, len);
            JSONPair_t item;
            for(int i = 0; root.Next(item); i++)
            {
                if(i < kMaxModes && item.jsonType == JSONArray)
                    ParseMode(item, i, numcontrols);
                else if(i == 3 && item.jsonType == JSONObject)
                    ParseSettings(item);
            }
            ret = Result::OK;
        }

        WriteWholeFile(buffer, size);

        return ret;
    }

    bool IsValid(size_t mode, size_t bank, size_t slot)
    {
        slot -= 1;
        if(slot > kMaxSlots || mode >= kMaxModes || bank >= kMaxBanks)
            return false;

        if (slot == 14)
            return chompi_valid;

        return values_valid[mode][bank][slot];
    }


    float *Values(size_t mode, size_t bank, size_t slot)
    {
        return slot == 15 ? chompi_value : values[mode][bank][slot - 1];
    }

    void Invalidate(uint8_t mode, uint8_t bank, uint8_t slot)
    {
        slot -= 1;
        if(slot >= kMaxSlots || mode >= kMaxModes || bank >= kMaxBanks)
            return;

        values_valid[mode][bank][slot] = false;

        updated = true;
    }

    void Save(uint8_t mode, uint8_t bank, uint8_t slot)
    {
        slot -= 1;
        if(slot >= kMaxSlots || mode >= kMaxModes || bank >= kMaxBanks)
            return;

        for(size_t i = 0; i < kMaxControls; i++)
        {
            values[mode][bank][slot][i] = chompi_value[i];
        }
        values_valid[mode][bank][slot] = chompi_valid;

        updated = true;
    }

    void Copy(uint8_t mode_src, uint8_t bank_src, uint8_t slot_src, 
              uint8_t mode_trg, uint8_t bank_trg, uint8_t slot_trg)
    {
        slot_trg -= 1;
        slot_src -= 1;
        if(slot_src > kMaxSlots || mode_src >= kMaxModes || bank_src >= kMaxBanks)
            return;

        if(slot_trg > kMaxSlots || mode_trg >= kMaxModes || bank_trg >= kMaxBanks)
            return;

        float* src = slot_src == 14 ? chompi_value : values[mode_src][bank_src][slot_src];
        float* dest = slot_trg == 14 ? chompi_value : values[mode_trg][bank_trg][slot_trg];
        for(size_t i = 0; i < kMaxControls; i++)
        {
            dest[i] = src[i];
        }

        bool valid = slot_src == 14 ? chompi_valid : values_valid[mode_src][bank_src][slot_src];
        if(slot_trg == 14)
            chompi_valid = valid;
        else
            values_valid[mode_trg][bank_trg][slot_trg] = valid;
    
        updated = true;
    }

    float values[kMaxModes][kMaxBanks][kMaxSlots][kMaxControls];
    float volume;
    bool values_valid[kMaxModes][kMaxBanks][kMaxSlots];

    float chompi_value[kMaxControls]; // only store the chompi settings in memory
    bool chompi_valid;

    private:




    // Appends at written_, so writing the file takes one pass.
    void StrAppend(char* buffer, char* append)
    {
        const size_t app_len = strlen(append);
        memcpy(buffer + written_, append, app_len + 1);
        written_ += app_len;
    }
    size_t written_ = 0;

    static const char *ModeName(int mode) { return mode ? "cubbi" : "jammi"; }

    class Items
    {
      public:
        Items(const char *buf, size_t len) : buf_(buf), len_(len) {}
        Items(const JSONPair_t &collection) : buf_(collection.value), len_(collection.valueLength) {}
        bool Next(JSONPair_t &item) { return JSON_Iterate(buf_, len_, &start_, &next_, &item) == JSONSuccess; }

      private:
        const char *buf_;
        size_t len_;
        size_t start_ = 0, next_ = 0;
    };

    static bool Is(const JSONPair_t &item, const char *key)
    {
        return item.key && item.keyLength == strlen(key) && memcmp(item.key, key, item.keyLength) == 0;
    }

    static float Unit(double v) { return std::max(0., std::min(1., v)); }

    void ParseMode(const JSONPair_t &banks, int mode, int numcontrols)
    {
        Items bank_items(banks);
        JSONPair_t bank_slots, slot_values;
        for(int bank = 0; bank < kMaxBanks && bank_items.Next(bank_slots); bank++)
        {
            if(bank_slots.jsonType != JSONArray)
                continue;
            Items slot_items(bank_slots);
            for(int slot = 0; slot < kMaxSlots && slot_items.Next(slot_values); slot++)
                if(slot_values.jsonType == JSONArray)
                    ParseSlot(slot_values, mode, bank, slot, numcontrols);
        }
    }

    // A slot counts only when the flag after its values is true.
    void ParseSlot(const JSONPair_t &slot_values, int mode, int bank, int slot, int numcontrols)
    {
        float v[kFileControls];
        bool have[kFileControls] = {};
        bool valid = false;
        Items items(slot_values);
        JSONPair_t item;
        for(int k = 0; k <= numcontrols && items.Next(item); k++)
        {
            if(k < numcontrols)
            {
                v[k] = atof(item.value) / 1000.;
                have[k] = true;
            }
            else
                valid = item.valueLength == 4 && memcmp(item.value, "true", 4) == 0;
        }
        values_valid[mode][bank][slot] = valid;
        if(valid)
            for(int k = 0; k < numcontrols; k++)
                if(have[k])
                    values[mode][bank][slot][k] = v[k];
    }

    void ParseSettings(const JSONPair_t &settings)
    {
        Items items(settings);
        JSONPair_t item;
        while(items.Next(item))
        {
            if(Is(item, "volume"))
                volume = Unit(atof(item.value) / 1000.);
            else if(Is(item, "slots") && item.jsonType == JSONObject)
                ParseSlotSettings(item);
        }
    }

    void ParseSlotSettings(const JSONPair_t &slots)
    {
        Items slot_items(slots);
        JSONPair_t slot_settings, setting;
        while(slot_items.Next(slot_settings))
        {
            int mode, bank, slot;
            if(!SlotOfKey(slot_settings, mode, bank, slot) || !values_valid[mode][bank][slot])
                continue;
            Items settings(slot_settings);
            while(settings.Next(setting))
            {
                if(Is(setting, "legato"))
                    values[mode][bank][slot][kLegato] = atoi(setting.value) != 0;
                else if(Is(setting, "glide"))
                    values[mode][bank][slot][kGlide] = Unit(atof(setting.value));
            }
        }
    }

    // Accepts only a key as the writer spells it, like "jammi-a-3".
    static bool SlotOfKey(const JSONPair_t &item, int &mode, int &bank, int &slot)
    {
        if(!item.key || item.keyLength < 9 || item.keyLength > 10)
            return false;
        for(mode = 0; mode < kMaxModes; mode++)
            if(memcmp(item.key, ModeName(mode), 5) == 0)
                break;
        bank = BankOfLetter(item.key[6]);
        slot = atoi(item.key + 8) - 1;
        if(mode >= kMaxModes || bank >= kMaxBanks || slot < 0 || slot >= kMaxSlots)
            return false;
        char name[12];
        snprintf(name, sizeof(name), "%s-%c-%d", ModeName(mode), kBanks[bank].letter, slot + 1);
        return strlen(name) == item.keyLength && memcmp(name, item.key, item.keyLength) == 0;
    }

        bool updated;
        const float* defaults_;
};
} // namespace chompi
#pragma GCC pop_options
