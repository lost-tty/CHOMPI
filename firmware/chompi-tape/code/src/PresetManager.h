#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include "FileStreamingManager.h"
#include "core_json.h"

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
    static constexpr int kMaxBanks = 5; // a, b, c, d, e
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

                buffer[strlen(buffer) - 1] = '\0';
                sprintf(append,"],");
                StrAppend(buffer, append);
            }

            buffer[strlen(buffer) - 1] = '\0';
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
                            'a' + bank, slot + 1, v[kLegato] > .5f, double(v[kGlide]));
                    StrAppend(buffer, append);
                    first = false;
                }
        strcpy(append, "}}");
        StrAppend(buffer, append);
        strcpy(append, "]"),
        StrAppend(buffer, append);

        return Result::OK;
    }

    /** loads the JSON file, storing in name/value pairs for all keys */
    Result Parse(char* buffer, size_t size)
    {
        updated = true;
        JSONStatus_t json_res;

        Result ret = Result::ERR_INVALID_JSON;
        
        size_t len = strlen(buffer);
        json_res = JSON_Validate(buffer, len);
        if(json_res == JSONSuccess)
        {
            char   query[51];
            char*  value;
            size_t value_len;

            /** Parse version number. No version number == version 1*/
            sprintf(query, "[2]");
            json_res = JSON_Search(
                buffer, len, query, strlen(query), &value, &value_len);

            uint8_t numcontrols = kFileControls;
            if(json_res != JSONSuccess) // V1
                numcontrols = 7;

            sprintf(query, "[3].volume");
            if(JSON_Search(buffer, len, query, strlen(query), &value, &value_len) == JSONSuccess)
                volume = std::max(0., std::min(1., atof(value) / 1000.));
            char *slots = nullptr;
            size_t slots_len = 0;
            sprintf(query, "[3].slots");
            if(JSON_Search(buffer, len, query, strlen(query), &value, &value_len) == JSONSuccess)
            {
                slots = value;
                slots_len = value_len;
            }

            /** Get module presets */
            for(int mode = 0; mode < kMaxModes; mode++){
                for(int bank = 0; bank < kMaxBanks; bank++)
                {
                    for(int slot = 0; slot < kMaxSlots; slot++)
                    {
                        // valid?
                        sprintf(query, "[%d][%d][%d][%d]", mode, bank, slot, numcontrols);
                        json_res = JSON_Search(
                            buffer, len, query, strlen(query), &value, &value_len);
                        
                        char save = value[4];
                        value[4] = '\0';
                        bool valid = strcmp(value, "true") == 0;
                        value[4] = save;

                        if(json_res == JSONSuccess && valid)
                        {
                            for(int ctrl = 0; ctrl < numcontrols; ctrl++)
                            {
                                /** and now check for the value */
                                sprintf(query, "[%d][%d][%d][%d]",mode, bank, slot, ctrl);

                                json_res = JSON_Search(
                                    buffer, len, query, strlen(query), &value, &value_len);

                                if(json_res == JSONSuccess)
                                {
                                    values[mode][bank][slot][ctrl] = atof(value) / 1000.;
                                }
                            }
                            values_valid[mode][bank][slot] = true;
                            if(slots)
                            {
                                sprintf(query, "%s-%c-%d.legato", ModeName(mode), 'a' + bank, slot + 1);
                                if(JSON_Search(slots, slots_len, query, strlen(query), &value, &value_len) == JSONSuccess)
                                    values[mode][bank][slot][kLegato] = atoi(value) != 0;
                                sprintf(query, "%s-%c-%d.glide", ModeName(mode), 'a' + bank, slot + 1);
                                if(JSON_Search(slots, slots_len, query, strlen(query), &value, &value_len) == JSONSuccess)
                                    values[mode][bank][slot][kGlide] = std::max(0., std::min(1., atof(value)));
                            }
                        }
                        else
                        {
                            values_valid[mode][bank][slot] = false;
                        }
                    }
                }
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




    void StrAppend(char* buffer, char* append)
    {
        size_t len = strlen(buffer);
        size_t app_len = strlen(append);

        for(size_t i = 0; i < app_len; i++)
        {
            buffer[len + i] = append[i];
        }
        buffer[len + app_len] = '\0';
    }

    static const char *ModeName(int mode) { return mode ? "cubbi" : "jammi"; }

        bool updated;
        const float* defaults_;
};
} // namespace chompi
#pragma GCC pop_options
