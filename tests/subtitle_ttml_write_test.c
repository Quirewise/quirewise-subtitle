/* TTML writer: SRT -> TTML -> SRT round-trip.
 *
 * Cue begin/end times are written as HH:MM:SS.mmm, text is XML-escaped, and
 * the TTML reader accepts the produced document again.
 */

#include <quirewise/subtitle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!data || !needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static const char srt[] =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Quirewise sample subtitle\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:06,000\n"
        "A & B <tag>\n";
    QwSubtitle *document = NULL;
    QwSubtitle *round_trip = NULL;
    uint8_t *ttml = NULL;
    size_t ttml_size = 0u;
    QwConvertReport report;
    QwSubtitleInfo info;

    memset(&report, 0, sizeof(report));
    CHECK(qw_subtitle_parse(srt, sizeof(srt) - 1u, QW_SUBTITLE_INPUT_SRT,
                            &document) == QW_SUBTITLE_OK);
    CHECK(document != NULL);
    CHECK(qw_subtitle_write(document, QW_FORMAT_TTML, NULL, &ttml,
                            &ttml_size, &report) == QW_SUBTITLE_OK);
    CHECK(ttml != NULL && ttml_size > 100u);
    CHECK(contains(ttml, ttml_size, "<tt xmlns=\"http://www.w3.org/ns/ttml\""));
    CHECK(contains(ttml, ttml_size,
                   "<p begin=\"00:00:01.000\" end=\"00:00:03.500\">"
                   "Quirewise sample subtitle</p>"));
    CHECK(contains(ttml, ttml_size, "A &amp; B &lt;tag&gt;"));
    CHECK(qw_subtitle_parse(ttml, ttml_size, QW_SUBTITLE_INPUT_TTML,
                            &round_trip) == QW_SUBTITLE_OK);
    CHECK(round_trip != NULL);
    memset(&info, 0, sizeof(info));
    CHECK(qw_subtitle_get_info(round_trip, &info) == QW_SUBTITLE_OK);
    CHECK(info.cue_count == 2u);
    {
        QwSubtitleCueView cue;
        memset(&cue, 0, sizeof(cue));
        CHECK(qw_subtitle_get_cue(round_trip, 0u, &cue) == QW_SUBTITLE_OK);
        CHECK(cue.start_milliseconds == 1000u);
        CHECK(cue.end_milliseconds == 3500u);
        CHECK(cue.text_size == strlen("Quirewise sample subtitle"));
        CHECK(memcmp(cue.text, "Quirewise sample subtitle",
                     cue.text_size) == 0);
    }
    {
        QwSubtitleCueView cue;
        memset(&cue, 0, sizeof(cue));
        CHECK(qw_subtitle_get_cue(round_trip, 1u, &cue) == QW_SUBTITLE_OK);
        CHECK(cue.text_size == strlen("A & B <tag>"));
        CHECK(memcmp(cue.text, "A & B <tag>", cue.text_size) == 0);
    }
    qw_subtitle_free(round_trip);
    qw_subtitle_free(document);
    qw_subtitle_buffer_free(ttml);
    puts("Quirewise TTML writer tests passed");
    return 0;
}
