/*
 * SRT parsing through the public subtitle API: cue count, timing and text.
 */

#include <quirewise/subtitle.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static const char srt[] =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Hello subtitle\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:05,000\n"
        "Second line\n";
    QwSubtitle *document = NULL;
    QwSubtitleInfo info;
    QwSubtitleCueView cue;

    CHECK(qw_subtitle_parse(srt, sizeof(srt) - 1u, QW_SUBTITLE_INPUT_SRT,
                            &document) == QW_SUBTITLE_OK);
    CHECK(document != NULL);
    memset(&info, 0, sizeof(info));
    CHECK(qw_subtitle_get_info(document, &info) == QW_SUBTITLE_OK);
    CHECK(info.cue_count == 2u);

    memset(&cue, 0, sizeof(cue));
    CHECK(qw_subtitle_get_cue(document, 0u, &cue) == QW_SUBTITLE_OK);
    CHECK(cue.start_milliseconds == 1000u);
    CHECK(cue.end_milliseconds == 3500u);
    CHECK(cue.text_size == strlen("Hello subtitle"));
    CHECK(memcmp(cue.text, "Hello subtitle", cue.text_size) == 0);
    CHECK(cue.identifier != NULL && cue.identifier_size == 1u &&
          cue.identifier[0] == '1');

    qw_subtitle_free(document);

    /* Malformed input is rejected with a status, not a crash. */
    document = NULL;
    CHECK(qw_subtitle_parse("not a subtitle", 14u, QW_SUBTITLE_INPUT_SRT,
                            &document) != QW_SUBTITLE_OK);
    return 0;
}
