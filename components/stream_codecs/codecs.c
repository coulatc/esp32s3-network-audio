#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
