/*
 * G.711 (A-law / mu-law) <-> linear16 conversion for the 8 kHz media path.
 * Thin wrappers over spandsp's inline g711 routines so the rest of the code
 * stays independent of spandsp.
 */
#ifndef SOFTMODEM_AUDIO_H_
#define SOFTMODEM_AUDIO_H_

#include <stdint.h>
#include <stddef.h>

typedef enum {
    CODEC_PCMU = 0,   /* RTP PT 0  - mu-law */
    CODEC_PCMA = 8    /* RTP PT 8  - A-law  */
} codec_t;

/* Encode linear16 -> G.711 (len samples -> len bytes). */
void audio_encode(codec_t codec, const int16_t *pcm, uint8_t *out, size_t len);

/* Decode G.711 -> linear16 (len bytes -> len samples). */
void audio_decode(codec_t codec, const uint8_t *in, int16_t *pcm, size_t len);

codec_t     audio_codec_from_name(const char *name);  /* "PCMA"/"PCMU" */
const char *audio_codec_name(codec_t codec);

#endif /* SOFTMODEM_AUDIO_H_ */
