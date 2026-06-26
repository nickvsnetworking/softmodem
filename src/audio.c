#include "audio.h"

#include <string.h>
#include <strings.h>
#include "spandsp.h"

void audio_encode(codec_t codec, const int16_t *pcm, uint8_t *out, size_t len) {
    if (codec == CODEC_PCMA)
        for (size_t i = 0; i < len; i++) out[i] = linear_to_alaw(pcm[i]);
    else
        for (size_t i = 0; i < len; i++) out[i] = linear_to_ulaw(pcm[i]);
}

void audio_decode(codec_t codec, const uint8_t *in, int16_t *pcm, size_t len) {
    if (codec == CODEC_PCMA)
        for (size_t i = 0; i < len; i++) pcm[i] = alaw_to_linear(in[i]);
    else
        for (size_t i = 0; i < len; i++) pcm[i] = ulaw_to_linear(in[i]);
}

codec_t audio_codec_from_name(const char *name) {
    if (name && (!strcasecmp(name, "PCMA") || !strcasecmp(name, "alaw")))
        return CODEC_PCMA;
    return CODEC_PCMU;
}

const char *audio_codec_name(codec_t codec) {
    return codec == CODEC_PCMA ? "PCMA" : "PCMU";
}
