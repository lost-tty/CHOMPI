#pragma GCC push_options
#pragma GCC optimize ("Os") // cold code, built for size so all of it fits in SRAM
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core_json.h"
#include "Banks.h"

namespace chompi
{
    // OP-1 sample format: an AIFF with the op-1 JSON in an APPL chunk. A drum kit has 24 slices.
    static constexpr uint32_t kKitSlices = 24;
    // op-1 positions run to 2^31 over the longest sample: 12 s at 44.1 kHz, 20 s on the OP-1 field
    static constexpr uint32_t kOp1Frames      = 12 * 44100;
    static constexpr uint32_t kOp1FieldFrames = 20 * 44100;

    inline bool IsAiffName(const char *name)
    {
        const size_t n = strlen(name);
        return n > 4 && !strcmp(name + n - 4, ".aif");
    }

    struct AiffInfo
    {
        uint32_t data_offset;   // first byte of audio
        uint32_t frames;
        uint8_t  channels;
        bool     little_endian; // AIFF-C 'sowt', plain AIFF is big-endian
        float    rate;
        bool     kit;           // op-1 drum metadata with slices
        uint32_t start[kKitSlices], end[kKitSlices]; // in frames
    };

    namespace aiff
    {
        inline uint32_t Be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

        // a number under key, the document must be valid
        inline bool Number(const char *json, size_t len, const char *key, size_t key_len, int32_t &value)
        {
            const char *out;
            size_t out_len;
            char num[16];
            if(JSON_SearchConst(json, len, key, key_len, &out, &out_len, NULL) != JSONSuccess || out_len >= sizeof(num))
                return false;
            memcpy(num, out, out_len);
            num[out_len] = '\0';
            value = strtol(num, NULL, 10);
            return true;
        }

        // the n-th number of a JSON array, the document must be valid
        inline bool ArrayValue(const char *json, size_t len, const char *key, uint32_t n, int32_t &value)
        {
            char query[16];
            size_t q = strlen(key);
            memcpy(query, key, q);
            query[q++] = '[';
            if(n >= 10)
                query[q++] = '0' + n / 10;
            query[q++] = '0' + n % 10;
            query[q++] = ']';

            const char *out;
            size_t out_len;
            if(JSON_SearchConst(json, len, query, q, &out, &out_len, NULL) != JSONSuccess)
                return false;
            char num[16];
            if(out_len >= sizeof(num))
                return false;
            memcpy(num, out, out_len);
            num[out_len] = '\0';
            value = strtol(num, NULL, 10);
            return true;
        }

        inline void ReadKit(char *json, size_t len, AiffInfo &info)
        {
            const char *type;
            size_t type_len;
            if(JSON_Validate(json, len) != JSONSuccess
               || JSON_SearchConst(json, len, "type", 4, &type, &type_len, NULL) != JSONSuccess
               || type_len != 4 || memcmp(type, "drum", 4))
                return;

            // only the field's format has a stereo key
            const char *stereo;
            size_t stereo_len;
            const uint64_t full = JSON_SearchConst(json, len, "stereo", 6, &stereo, &stereo_len, NULL) == JSONSuccess
                                      ? kOp1FieldFrames
                                      : kOp1Frames;
            for(uint32_t i = 0; i < kKitSlices; i++)
            {
                int32_t s, e;
                if(!ArrayValue(json, len, "start", i, s) || !ArrayValue(json, len, "end", i, e))
                    return;
                info.start[i] = s > 0 ? uint32_t(s * full >> 31) : 0;
                info.end[i]   = e > 0 ? uint32_t(e * full >> 31) : 0;
            }
            info.kit = true;
        }
    }

    extern AiffInfo slot_info;    // the loaded JAMMI slot: its format, a kit's slices; its voices share it
    extern uint16_t kit_slots[kNumBanks]; // per bank, the JAMMI slots that play an .aif instead of a .wav, bit = slot

    // Reads the chunks of an AIFF or AIFF-C through read(pos, dst, n). json is scratch for the op-1 data.
    // False if it isn't 16-bit PCM audio this can play.
    template <typename Read>
    bool ParseAiff(Read read, uint32_t file_size, AiffInfo &info, char *json, size_t json_size)
    {
        memset(&info, 0, sizeof(info));
        uint8_t h[26];
        if(!read(0, h, 12) || memcmp(h, "FORM", 4) || (memcmp(h + 8, "AIFF", 4) && memcmp(h + 8, "AIFC", 4)))
            return false;

        bool comm = false, ssnd = false;
        for(uint32_t pos = 12; pos + 8 <= file_size;)
        {
            if(!read(pos, h, 8))
                return false;
            const uint32_t size = aiff::Be32(h + 4);
            const uint32_t body = pos + 8;

            if(!memcmp(h, "COMM", 4) && size >= 18)
            {
                if(!read(body, h, size >= 22 ? 22 : 18))
                    return false;
                info.channels = h[1];
                info.frames   = aiff::Be32(h + 2);
                const int exponent = ((h[8] & 0x7f) << 8 | h[9]) - 16383 - 31;
                if(exponent >= 0 || exponent < -31)
                    return false;
                info.rate = float(aiff::Be32(h + 10)) / float(1u << -exponent);
                if(h[6] != 0 || h[7] != 16 || info.rate < 4000.f || info.rate > 192000.f) // 16 bits, 4 to 192 kHz
                    return false;
                if(size >= 22) // AIFF-C: twos/NONE are big-endian, sowt little-endian, the rest is compressed
                {
                    info.little_endian = !memcmp(h + 18, "sowt", 4);
                    if(!info.little_endian && memcmp(h + 18, "NONE", 4) && memcmp(h + 18, "twos", 4))
                        return false;
                }
                comm = true;
            }
            else if(!memcmp(h, "SSND", 4) && size >= 8)
            {
                if(!read(body, h, 4))
                    return false;
                info.data_offset = body + 8 + aiff::Be32(h);
                ssnd = true;
            }
            else if(!memcmp(h, "APPL", 4) && size > 4 && size <= json_size)
            {
                if(!read(body, json, size))
                    return false;
                size_t len = memcmp(json, "op-1", 4) ? 0 : size - 4; // another app's data
                while(len && json[3 + len] != '}') // the op-1 tools pad after the JSON
                    len--;
                if(len)
                    aiff::ReadKit(json + 4, len, info);
            }
            const uint64_t next = uint64_t(body) + size + (size & 1);
            if(next > file_size)
                break;
            pos = uint32_t(next);
        }
        if(!comm || !ssnd || (info.channels != 1 && info.channels != 2) || info.data_offset >= file_size)
            return false;
        const uint32_t frames = (file_size - info.data_offset) / (info.channels * 2); // a cut short file plays what is there
        if(info.frames > frames)
            info.frames = frames;
        return info.frames > 0;
    }
}

#pragma GCC pop_options
