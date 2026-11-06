#include <quirewise/subtitle.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FUZZ_SEED UINT32_C(0x51f04499)
#define FUZZ_RUNS 200u

static uint32_t rng_state = FUZZ_SEED;

static uint32_t next_random(void)
{
    uint32_t value = rng_state;
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    rng_state = value;
    return value;
}

static size_t mutate(const char *seed, char *buffer, size_t capacity)
{
    size_t length = strlen(seed);
    size_t index;
    unsigned int flips = 1u + next_random() % 4u;
    if (length + 1u > capacity) length = capacity - 1u;
    memcpy(buffer, seed, length);
    buffer[length] = '\0';
    if (next_random() % 8u == 0u && length > 1u)
        length = 1u + next_random() % length;
    for (index = 0u; index < flips && length; ++index) {
        size_t position = next_random() % length;
        buffer[position] = (char)(buffer[position] ^
                                  (uint8_t)(1u << (next_random() % 8u)));
    }
    return length;
}

typedef struct Seed {
    QwSubtitleInputFormat format;
    const char *text;
} Seed;

int main(void)
{
    static const Seed seeds[] = {
        {QW_SUBTITLE_INPUT_SRT,
         "1\r\n00:00:00,000 --> 00:00:01,000\r\nHello\r\n"},
        {QW_SUBTITLE_INPUT_VTT,
         "WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nHello\n"},
        {QW_SUBTITLE_INPUT_LRC, "[00:00.00]Hello\n[00:01.00]World\n"},
        {QW_SUBTITLE_INPUT_ASS,
         "[Script Info]\nTitle: t\n[Events]\n"
         "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,Hello\n"},
        {QW_SUBTITLE_INPUT_SSA,
         "[Script Info]\n[Events]\n"
         "Dialogue: Marked=0,0:00:00.00,0:00:01.00,Default,,0000,0000,0000,,Hello\n"},
        {QW_SUBTITLE_INPUT_SAMI,
         "<SAMI><BODY><SYNC Start=0><P>Hello</P></SYNC></BODY></SAMI>"},
        {QW_SUBTITLE_INPUT_MICRODVD,
         "{0}{25}Hello\n{25}{50}World\n"},
        {QW_SUBTITLE_INPUT_SCC,
         "Scenarist_SCC V1.0\n\n00:00:00:00\t9420 9420 9470 9470\n"},
    };
    char buffer[1024];
    size_t index;
    size_t run;
    size_t ok = 0u;
    size_t rejected = 0u;
    for (index = 0u; index < sizeof(seeds) / sizeof(seeds[0]); ++index) {
        for (run = 0u; run < FUZZ_RUNS; ++run) {
            QwSubtitle *document = NULL;
            size_t size = mutate(seeds[index].text, buffer, sizeof(buffer));
            QwSubtitleStatus status = qw_subtitle_parse(
                buffer, size, seeds[index].format, &document);
            if (status == QW_SUBTITLE_OK && document) {
                ++ok;
                qw_subtitle_free(document);
            } else {
                ++rejected;
            }
        }
    }
    printf("Subtitle fuzz smoke: %zu ok, %zu rejected, seed=0x%08x runs=%u\n",
           ok, rejected, FUZZ_SEED, FUZZ_RUNS);
    return 0;
}
