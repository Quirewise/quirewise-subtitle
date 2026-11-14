#include "quirewise/subtitle.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s at line %d\n", #condition, __LINE__); \
        return 1; \
    } \
} while (0)

static const char *TTML_DOCUMENT =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<tt xmlns=\"http://www.w3.org/ns/ttml\" "
    "xmlns:tts=\"http://www.w3.org/ns/ttml#styling\" xml:lang=\"en\">"
    "<head><styling><style xml:id=\"s1\" tts:color=\"white\"/></styling></head>"
    "<body><div>"
    "<p begin=\"00:00:01.000\" end=\"00:00:03.500\" xml:id=\"c1\">"
    "Hello <span tts:fontStyle=\"italic\">world</span><br/>"
    "Second line &amp; more</p>"
    "<p begin=\"4s\" dur=\"1.5s\">Second cue</p>"
    "<p begin=\"00:00:10:15\" end=\"00:00:11:00\">Frame cue</p>"
    "</div></body></tt>";

static int parse(const char *text, QwSubtitle **out)
{
    return (int)qw_subtitle_parse(text, strlen(text),
                                  QW_SUBTITLE_INPUT_TTML, out);
}

int main(void)
{
    QwSubtitle *document = NULL;
    QwSubtitleInfo info;
    QwSubtitleCueView cue;

    CHECK(parse(TTML_DOCUMENT, &document) == QW_SUBTITLE_OK);
    CHECK(document != NULL);
    memset(&info, 0, sizeof(info));
    CHECK(qw_subtitle_get_info(document, &info) == QW_SUBTITLE_OK);
    CHECK(info.input_format == QW_SUBTITLE_INPUT_TTML);
    CHECK(info.cue_count == 3u);
    CHECK(strcmp(qw_subtitle_format_name(info.input_format), "ttml") == 0);

    memset(&cue, 0, sizeof(cue));
    CHECK(qw_subtitle_get_cue(document, 0u, &cue) == QW_SUBTITLE_OK);
    CHECK(cue.start_milliseconds == 1000u);
    CHECK(cue.end_milliseconds == 3500u);
    CHECK(cue.end_time_explicit == 1);
    CHECK(cue.identifier_size == 2u && memcmp(cue.identifier, "c1", 2u) == 0);
    CHECK(cue.text_size == strlen("Hello world\nSecond line & more"));
    CHECK(memcmp(cue.text, "Hello world\nSecond line & more", cue.text_size) == 0);

    memset(&cue, 0, sizeof(cue));
    CHECK(qw_subtitle_get_cue(document, 1u, &cue) == QW_SUBTITLE_OK);
    CHECK(cue.start_milliseconds == 4000u);
    CHECK(cue.end_milliseconds == 5500u);
    CHECK(cue.text_size == strlen("Second cue"));
    CHECK(memcmp(cue.text, "Second cue", cue.text_size) == 0);

    /* 15 frames at the default 30 fps is 500 ms. */
    memset(&cue, 0, sizeof(cue));
    CHECK(qw_subtitle_get_cue(document, 2u, &cue) == QW_SUBTITLE_OK);
    CHECK(cue.start_milliseconds == 10500u);
    CHECK(cue.end_milliseconds == 11000u);
    qw_subtitle_free(document);

    /* Malformed timing must be rejected as a format error, not accepted. */
    CHECK(parse("<tt><body><div><p begin=\"nope\">Bad</p></div></body></tt>",
                &document) == QW_SUBTITLE_FORMAT_ERROR);
    CHECK(document == NULL);
    CHECK(parse("<tt><body><div><p begin=\"1s\">No end</p></div></body></tt>",
                &document) == QW_SUBTITLE_FORMAT_ERROR);
    CHECK(document == NULL);

    /* A non-TTML XML document is unsupported rather than a format error. */
    CHECK(parse("<html><body><p>Not TTML</p></body></html>",
                &document) == QW_SUBTITLE_UNSUPPORTED);
    CHECK(document == NULL);

    /* No cues at all is a format error. */
    CHECK(parse("<tt xmlns=\"http://www.w3.org/ns/ttml\"><body/></tt>",
                &document) == QW_SUBTITLE_FORMAT_ERROR);
    CHECK(document == NULL);

    printf("TTML parse: 3 cues, span/br/entity/frames handled\n");
    return 0;
}
