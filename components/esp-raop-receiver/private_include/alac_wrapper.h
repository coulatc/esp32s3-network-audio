#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct alac_codec_s;
struct alac_codec_s *alac_create_decoder(int cookie_size, unsigned char *cookie,
    unsigned char *sample_size, unsigned *sample_rate, unsigned char *channels, unsigned *block_size);
void alac_delete_decoder(struct alac_codec_s *codec);
bool alac_to_pcm(struct alac_codec_s *codec, const unsigned char *input, unsigned input_len,
    unsigned char *output, char channels, unsigned *frames);

#ifdef __cplusplus
}
#endif
