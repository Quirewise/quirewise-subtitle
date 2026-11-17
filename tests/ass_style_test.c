/* ASS override tags survive an ASS-to-ASS round trip. */

#include <quirewise/subtitle.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ass[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
    "Effect, Text\n"
    "Dialogue: 0,0:00:01.00,0:00:02.00,Default,Alice,0,0,0,,"
    "{\\i1}Hello{\\i0} world\n";

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    QwSubtitle *document = NULL;
    QwSubtitleCueView cue;
    uint8_t *output = NULL;
    size_t output_size = 0u;
    QwConvertReport report;

    memset(&report, 0, sizeof(report));
    if (qw_subtitle_parse(ass, strlen(ass), QW_SUBTITLE_INPUT_ASS, &document) !=
            QW_SUBTITLE_OK ||
        !document) {
        fputs("FAIL: styled ASS fixture did not parse\n", stderr);
        return 1;
    }
    memset(&cue, 0, sizeof(cue));
    if (qw_subtitle_get_cue(document, 0u, &cue) != QW_SUBTITLE_OK ||
        !cue.text || cue.text_size != 11u ||
        memcmp(cue.text, "Hello world", 11u) != 0) {
        fputs("FAIL: ASS override tags were not stripped from cue text\n",
              stderr);
        qw_subtitle_free(document);
        return 1;
    }
    memset(&report, 0, sizeof(report));
    if (qw_subtitle_write(document, QW_FORMAT_ASS, NULL, &output, &output_size,
                          &report) != QW_SUBTITLE_OK ||
        !output || !output_size) {
        fprintf(stderr, "FAIL: ASS write (%s)\n", report.note);
        qw_subtitle_free(document);
        return 1;
    }
    if (!contains(output, output_size, "{\\i1}Hello{\\i0} world")) {
        fputs("FAIL: ASS override tags were not written back\n", stderr);
        qw_subtitle_buffer_free(output);
        qw_subtitle_free(document);
        return 1;
    }
    qw_subtitle_buffer_free(output);
    qw_subtitle_free(document);
    puts("Quirewise ASS override tag tests passed");
    return 0;
}
