/*
 * Subtitle writer tests: one parsed SRT cue list is written in every target
 * format, the bytes are checked for the expected marker, and formats with a
 * parser are re-read to confirm the cue text survives.
 */

#include <quirewise/subtitle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static const char srt_text[] =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Hello, world!\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:06,000\n"
        "Second cue\n";
    static const struct {
        QwOutputFormat output;
        QwSubtitleInputFormat reparse;
        const char *marker;
    } cases[] = {
        { QW_FORMAT_SRT, QW_SUBTITLE_INPUT_SRT, "Hello, world!" },
        { QW_FORMAT_VTT, QW_SUBTITLE_INPUT_VTT, "WEBVTT" },
        { QW_FORMAT_LRC, QW_SUBTITLE_INPUT_LRC, "Hello, world!" },
        { QW_FORMAT_SUB, QW_SUBTITLE_INPUT_MICRODVD, "Hello, world!" },
        { QW_FORMAT_ASS, QW_SUBTITLE_INPUT_ASS, "Dialogue:" },
        { QW_FORMAT_SSA, QW_SUBTITLE_INPUT_SSA, "Dialogue:" },
        { QW_FORMAT_SAMI, QW_SUBTITLE_INPUT_SAMI, "Hello, world!" },
        { QW_FORMAT_SCC, QW_SUBTITLE_INPUT_SCC, "Scenarist_SCC" }
    };
    QwSubtitle *document = NULL;
    QwSubtitleInfo info;
    size_t index;

    memset(&info, 0, sizeof(info));
    if (qw_subtitle_parse(srt_text, strlen(srt_text), QW_SUBTITLE_INPUT_SRT,
                          &document) != QW_SUBTITLE_OK ||
        !document)
        fail("SRT parse failed");
    if (qw_subtitle_get_info(document, &info) != QW_SUBTITLE_OK ||
        info.cue_count != 2u)
        fail("SRT cue count mismatch");

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        QwConvertReport report;
        uint8_t *output = NULL;
        size_t output_size = 0u;

        memset(&report, 0, sizeof(report));
        if (qw_subtitle_write(document, cases[index].output, NULL, &output,
                              &output_size, &report) != QW_SUBTITLE_OK ||
            !output || !output_size) {
            fprintf(stderr, "FAIL: %s writer (%s)\n",
                    qw_subtitle_format_name(cases[index].reparse),
                    report.note);
            exit(1);
        }
        if (!contains(output, output_size, cases[index].marker))
            fail("writer output lost the cue text");
        if (cases[index].reparse != QW_SUBTITLE_INPUT_TTML) {
            QwSubtitle *round = NULL;
            QwSubtitleInfo round_info;

            memset(&round_info, 0, sizeof(round_info));
            if (qw_subtitle_parse(output, output_size, cases[index].reparse,
                                  &round) != QW_SUBTITLE_OK ||
                !round)
                fail("writer output did not parse again");
            if (qw_subtitle_get_info(round, &round_info) != QW_SUBTITLE_OK ||
                round_info.cue_count != 2u)
                fail("re-read cue count mismatch");
            qw_subtitle_free(round);
        }
        qw_subtitle_buffer_free(output);
    }

    {
        QwConvertReport report;
        uint8_t *output = (uint8_t *)1;
        size_t output_size = 123u;

        memset(&report, 0, sizeof(report));
        if (qw_subtitle_write(document, QW_FORMAT_PDF, NULL, &output,
                              &output_size, &report) !=
            QW_SUBTITLE_UNSUPPORTED)
            fail("PDF target was accepted");
        if (output || output_size) fail("unsupported target produced output");
    }

    if (qw_subtitle_write(NULL, QW_FORMAT_SRT, NULL, NULL, NULL, NULL) !=
        QW_SUBTITLE_INVALID_ARGUMENT)
        fail("NULL arguments were accepted");
    qw_subtitle_buffer_free(NULL);
    qw_subtitle_free(document);
    puts("Quirewise subtitle writer tests passed");
    return 0;
}
