#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include "alac_wrapper.h"

struct alac_codec_s {
    ALACDecoder decoder;
    uint8_t input[2048] = {};
};

extern "C" alac_codec_s *alac_create_decoder(int cookie_size, unsigned char *cookie,
    unsigned char *sample_size, unsigned *sample_rate, unsigned char *channels, unsigned *block_size)
{
    auto *codec = new (std::nothrow) alac_codec_s;
    if (!codec) return nullptr;
    if (codec->decoder.Init(cookie, cookie_size) != ALAC_noErr) {
        delete codec;
        return nullptr;
    }
    const auto &config = codec->decoder.mConfig;
    *channels = config.numChannels;
    *sample_rate = config.sampleRate;
    *sample_size = config.bitDepth;
    *block_size = config.frameLength * config.numChannels * (config.bitDepth / 8);
    return codec;
}

extern "C" void alac_delete_decoder(alac_codec_s *codec)
{
    delete codec;
}

extern "C" bool alac_to_pcm(alac_codec_s *codec, const unsigned char *input, unsigned input_len,
    unsigned char *output, char channels, unsigned *frames)
{
    constexpr unsigned guard_bytes = 16;
    if (!codec || !input_len || input_len > sizeof(codec->input) - guard_bytes) return false;
    std::memcpy(codec->input, input, input_len);
    std::memset(codec->input + input_len, 0, guard_bytes);
    BitBuffer data;
    BitBufferInit(&data, codec->input, input_len);
    unsigned max_frames = codec->decoder.mConfig.frameLength;
    uint32_t decoded = 0;
    int status = codec->decoder.Decode(&data, output, max_frames, channels, &decoded);
    *frames = decoded;
    return status == ALAC_noErr &&
           *frames <= max_frames;
}
