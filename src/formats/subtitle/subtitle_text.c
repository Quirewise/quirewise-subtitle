#include "subtitle_format.h"

#include "../../core/text_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SUBTITLE_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    SUBTITLE_MAX_OUTPUT_BYTES = 64u * 1024u * 1024u
};

#define SUBTITLE_APPEND_LITERAL(buffer, literal) \
    qw_text_buffer_append((buffer), (literal), sizeof(literal) - 1u)

typedef struct SubtitleLine {
    const uint8_t *data;
    size_t size;
} SubtitleLine;

static int subtitle_utf8_valid(const uint8_t *text, size_t size)
{
    size_t position = 0u;
    while (position < size) {
        uint8_t first = text[position++];
        size_t continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t i;
        if (first < 0x80u) {
            if (first == 0u || (first < 0x20u && first != '\r' &&
                first != '\n' && first != '\t')) return 0;
            continue;
        }
        if ((first & 0xe0u) == 0xc0u) {
            continuation_count = 1u;
            codepoint = first & 0x1fu;
            minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuation_count = 2u;
            codepoint = first & 0x0fu;
            minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuation_count = 3u;
            codepoint = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return 0;
        }
        if (continuation_count > size - position) return 0;
        for (i = 0u; i < continuation_count; ++i) {
            uint8_t next = text[position++];
            if ((next & 0xc0u) != 0x80u) return 0;
            codepoint = (codepoint << 6) | (next & 0x3fu);
        }
        if (codepoint < minimum || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
            (codepoint < 0x20u && codepoint != '\r' && codepoint != '\n' &&
             codepoint != '\t')) return 0;
    }
    return 1;
}

static int subtitle_next_line(const uint8_t *data, size_t size,
                              size_t *position, SubtitleLine *line)
{
    size_t start;
    if (!data || !position || !line || *position >= size) return 0;
    start = *position;
    while (*position < size && data[*position] != '\r' &&
           data[*position] != '\n') ++*position;
    line->data = data + start;
    line->size = *position - start;
    if (*position < size && data[*position] == '\r') ++*position;
    if (*position < size && data[*position] == '\n') ++*position;
    return 1;
}

static int subtitle_line_blank(const SubtitleLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static int subtitle_line_has_arrow(const SubtitleLine *line)
{
    size_t i;
    if (!line || line->size < 3u) return 0;
    for (i = 0u; i + 2u < line->size; ++i) {
        if (line->data[i] == '-' && line->data[i + 1u] == '-' &&
            line->data[i + 2u] == '>') return 1;
    }
    return 0;
}

static int subtitle_read_digits(const uint8_t *data, size_t size,
                                size_t *position, size_t minimum_digits,
                                size_t maximum_digits, uint64_t *value,
                                size_t *digits_read)
{
    size_t start;
    uint64_t result = 0u;
    if (!data || !position || !value) return 0;
    start = *position;
    while (*position < size && data[*position] >= '0' &&
           data[*position] <= '9') {
        unsigned digit = (unsigned)(data[*position] - '0');
        if (*position - start >= maximum_digits ||
            result > (UINT64_MAX - digit) / 10u) return 0;
        result = result * 10u + digit;
        ++*position;
    }
    if (*position - start < minimum_digits) return 0;
    *value = result;
    if (digits_read) *digits_read = *position - start;
    return 1;
}

static int subtitle_parse_time(const uint8_t *data, size_t size,
                               QwInputFormat input_format, size_t *position,
                               uint64_t *out_milliseconds)
{
    uint64_t first;
    uint64_t second;
    uint64_t third;
    uint64_t milliseconds;
    size_t first_digits = 0u;
    size_t digits;
    uint64_t hours;
    uint64_t minutes;
    uint64_t seconds;
    if (!data || !position || !out_milliseconds) return 0;
    if (!subtitle_read_digits(data, size, position, 2u, 9u, &first,
                              &first_digits) ||
        *position >= size || data[(*position)++] != ':') return 0;
    if (!subtitle_read_digits(data, size, position, 2u, 2u, &second, NULL))
        return 0;
    if (*position < size && data[*position] == ':') {
        ++*position;
        if (!subtitle_read_digits(data, size, position, 2u, 2u, &third, NULL))
            return 0;
        hours = first;
        minutes = second;
        seconds = third;
    } else if (input_format == QW_FORMAT_VTT) {
        hours = 0u;
        minutes = first;
        seconds = second;
        first_digits = 2u;
    } else {
        return 0;
    }
    if (minutes > 59u || seconds > 59u ||
        (input_format == QW_FORMAT_VTT && hours == 0u && first_digits != 2u)) {
        return 0;
    }
    if (*position >= size ||
        data[(*position)++] != (input_format == QW_FORMAT_SRT ? ',' : '.') ||
        !subtitle_read_digits(data, size, position, 3u, 3u,
                              &milliseconds, &digits) || digits != 3u) return 0;
    if (hours > (UINT64_MAX / 3600000u) ||
        minutes > (UINT64_MAX - hours * 3600000u) / 60000u) return 0;
    *out_milliseconds = hours * 3600000u + minutes * 60000u +
                        seconds * 1000u + milliseconds;
    return 1;
}

static int subtitle_parse_timing_line(const SubtitleLine *line,
                                      QwInputFormat input_format,
                                      uint64_t *start_milliseconds,
                                      uint64_t *end_milliseconds,
                                      const uint8_t **settings,
                                      size_t *settings_size)
{
    size_t position = 0u;
    while (position < line->size &&
           (line->data[position] == ' ' || line->data[position] == '\t'))
        ++position;
    if (!subtitle_parse_time(line->data, line->size, input_format, &position,
                             start_milliseconds)) return 0;
    while (position < line->size &&
           (line->data[position] == ' ' || line->data[position] == '\t'))
        ++position;
    if (line->size - position < 3u || line->data[position] != '-' ||
        line->data[position + 1u] != '-' || line->data[position + 2u] != '>')
        return 0;
    position += 3u;
    while (position < line->size &&
           (line->data[position] == ' ' || line->data[position] == '\t'))
        ++position;
    if (!subtitle_parse_time(line->data, line->size, input_format, &position,
                             end_milliseconds) ||
        *start_milliseconds >= *end_milliseconds) return 0;
    while (position < line->size &&
           (line->data[position] == ' ' || line->data[position] == '\t'))
        ++position;
    if (input_format == QW_FORMAT_SRT) {
        if (position != line->size) return 0;
        *settings = NULL;
        *settings_size = 0u;
        return 1;
    }
    *settings = line->data + position;
    *settings_size = line->size - position;
    while (*settings_size && ((*settings)[*settings_size - 1u] == ' ' ||
                              (*settings)[*settings_size - 1u] == '\t')) {
        --*settings_size;
    }
    return 1;
}

static int subtitle_append_text_line(QwTextBuffer *text,
                                     const SubtitleLine *line,
                                     size_t *line_count)
{
    if (*line_count && !qw_text_buffer_append(text, "\n", 1u)) return 0;
    if (line->size && !qw_text_buffer_append(text, (const char *)line->data,
                                            line->size)) return 0;
    ++*line_count;
    return 1;
}

static int subtitle_collect_cue_text(const uint8_t *data, size_t size,
                                    size_t *position, QwTextBuffer *text)
{
    SubtitleLine line;
    size_t line_count = 0u;
    text->limit = SUBTITLE_MAX_INPUT_BYTES + 1u;
    while (subtitle_next_line(data, size, position, &line)) {
        if (subtitle_line_blank(&line)) break;
        if (!subtitle_append_text_line(text, &line, &line_count)) return 0;
    }
    return line_count != 0u;
}

static int subtitle_lrc_parse_timestamp(const uint8_t *data, size_t size,
                                        size_t *position,
                                        uint64_t *out_milliseconds)
{
    uint64_t minutes = 0u;
    uint64_t seconds;
    uint64_t fraction = 0u;
    uint64_t base;
    size_t digits = 0u;
    size_t fraction_digits = 0u;
    size_t cursor;
    if (!data || !position || !out_milliseconds || *position >= size ||
        data[(*position)++] != '[') return 0;
    cursor = *position;
    while (cursor < size && data[cursor] >= '0' && data[cursor] <= '9') {
        unsigned digit = (unsigned)(data[cursor] - '0');
        if (minutes > (UINT64_MAX - digit) / 10u) return 0;
        minutes = minutes * 10u + digit;
        ++cursor;
        ++digits;
    }
    if (!digits || cursor >= size || data[cursor++] != ':' ||
        size - cursor < 2u || data[cursor] < '0' || data[cursor] > '9' ||
        data[cursor + 1u] < '0' || data[cursor + 1u] > '9') return 0;
    seconds = (uint64_t)(data[cursor] - '0') * 10u +
              (uint64_t)(data[cursor + 1u] - '0');
    cursor += 2u;
    if (seconds >= 60u || cursor >= size) return 0;
    if (data[cursor] == '.') {
        ++cursor;
        while (cursor < size && data[cursor] >= '0' && data[cursor] <= '9' &&
               fraction_digits < 3u) {
            fraction = fraction * 10u + (uint64_t)(data[cursor] - '0');
            ++cursor;
            ++fraction_digits;
        }
        if (!fraction_digits ||
            (cursor < size && data[cursor] >= '0' && data[cursor] <= '9')) {
            return 0;
        }
        if (fraction_digits == 1u) fraction *= 100u;
        else if (fraction_digits == 2u) fraction *= 10u;
    }
    if (cursor >= size || data[cursor++] != ']') return 0;
    if (minutes > UINT64_MAX / 60000u) return 0;
    base = minutes * 60000u;
    seconds = seconds * 1000u + fraction;
    if (base > UINT64_MAX - seconds) return 0;
    *out_milliseconds = base + seconds;
    *position = cursor;
    return 1;
}

static int subtitle_lrc_metadata_line(const uint8_t *data, size_t size)
{
    static const char *const metadata_keys[] = {
        "ar", "al", "ti", "by", "re", "ve", "length", "offset",
        "au", "la", "kana", "tool"
    };
    size_t close = 0u;
    size_t colon = SIZE_MAX;
    size_t i;
    if (!data || size < 4u || data[0] != '[') return 0;
    while (close < size && data[close] != ']') {
        if (data[close] == ':' && colon == SIZE_MAX) colon = close;
        ++close;
    }
    if (close >= size || colon == SIZE_MAX || colon <= 1u) return 0;
    for (i = 0u; i < sizeof(metadata_keys) / sizeof(metadata_keys[0]); ++i) {
        size_t key_size = strlen(metadata_keys[i]);
        size_t j;
        if (key_size != colon - 1u) continue;
        for (j = 0u; j < key_size; ++j) {
            uint8_t ch = data[j + 1u];
            if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + ('a' - 'A'));
            if (ch != (uint8_t)metadata_keys[i][j]) break;
        }
        if (j == key_size) return 1;
    }
    return 0;
}

static int subtitle_lrc_text_starts_timestamp(const uint8_t *text, size_t size)
{
    return text && size > 1u && text[0] == '[' &&
           text[1] >= '0' && text[1] <= '9';
}

static int subtitle_parse_lrc_data(const void *input, size_t input_size,
                                   QwSubtitleDocument *out_document,
                                   QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    int skipped_tags = 0;
    int skipped_empty = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    SubtitleLine line;
    if (!input || !input_size || !out_document ||
        input_size > SUBTITLE_MAX_INPUT_BYTES) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, QW_FORMAT_LRC);
    if (!subtitle_utf8_valid(data, size)) {
        rc = QW_STATUS_ERR_DECODE;
        goto failure;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    while (subtitle_next_line(data, size, &position, &line)) {
        uint64_t *timestamps = NULL;
        size_t timestamp_count = 0u;
        size_t timestamp_capacity = 0u;
        size_t cursor = 0u;
        size_t text_size;
        size_t i;
        while (cursor < line.size &&
               (line.data[cursor] == ' ' || line.data[cursor] == '\t')) ++cursor;
        while (cursor < line.size && line.data[cursor] == '[' &&
               cursor + 1u < line.size && line.data[cursor + 1u] >= '0' &&
               line.data[cursor + 1u] <= '9') {
            uint64_t timestamp;
            uint64_t *grown;
            size_t capacity;
            if (timestamp_count >= QW_SUBTITLE_MAX_CUES ||
                !subtitle_lrc_parse_timestamp(line.data, line.size, &cursor,
                                               &timestamp)) {
                free(timestamps);
                goto failure;
            }
            if (timestamp_count == timestamp_capacity) {
                capacity = timestamp_capacity ? timestamp_capacity * 2u : 4u;
                if (capacity < timestamp_capacity) {
                    free(timestamps);
                    rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto failure;
                }
                if (capacity > QW_SUBTITLE_MAX_CUES)
                    capacity = QW_SUBTITLE_MAX_CUES;
                if (capacity <= timestamp_capacity ||
                    capacity > SIZE_MAX / sizeof(*grown)) {
                    free(timestamps);
                    rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                    goto failure;
                }
                grown = (uint64_t *)realloc(timestamps,
                                            capacity * sizeof(*grown));
                if (!grown) {
                    free(timestamps);
                    rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto failure;
                }
                timestamps = grown;
                timestamp_capacity = capacity;
            }
            timestamps[timestamp_count++] = timestamp;
            while (cursor < line.size &&
                   (line.data[cursor] == ' ' || line.data[cursor] == '\t'))
                ++cursor;
        }
        if (!timestamp_count) {
            if (subtitle_line_blank(&line)) continue;
            if (subtitle_lrc_metadata_line(line.data + cursor,
                                           line.size - cursor)) {
                skipped_tags = 1;
                continue;
            }
            goto failure;
        }
        text_size = line.size - cursor;
        while (text_size && (line.data[cursor + text_size - 1u] == ' ' ||
                             line.data[cursor + text_size - 1u] == '\t'))
            --text_size;
        if (!text_size) {
            skipped_empty = 1;
            free(timestamps);
            continue;
        }
        if (timestamp_count > QW_SUBTITLE_MAX_CUES -
                                  out_document->cue_count ||
            timestamp_count > (QW_SUBTITLE_MAX_CONTENT_BYTES -
                               out_document->content_bytes) / text_size) {
            free(timestamps);
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto failure;
        }
        for (i = 0u; i < timestamp_count; ++i) {
            uint64_t end;
            if (timestamps[i] > UINT64_MAX - 5000u) {
                free(timestamps);
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto failure;
            }
            end = timestamps[i] + 5000u;
            if (!qw_subtitle_document_add_cue(out_document, timestamps[i], end,
                    0, NULL, 0u, NULL, 0u, line.data + cursor, text_size)) {
                free(timestamps);
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
                goto failure;
            }
        }
        free(timestamps);
    }
    if (!qw_subtitle_document_validate(out_document)) goto failure;
    if (report) {
        const char *loss = skipped_tags && skipped_empty
            ? "LRC lyric timing and text were read; metadata tags and empty lyric lines were omitted; cue ends are inferred as five seconds"
            : skipped_tags
                ? "LRC lyric timing and text were read; metadata tags were omitted; cue ends are inferred as five seconds"
                : skipped_empty
                    ? "LRC lyric timing and text were read; empty lyric lines were omitted; cue ends are inferred as five seconds"
                    : "LRC lyric timing and UTF-8 text were read; cue ends are inferred as five seconds";
        (void)snprintf(report->note, sizeof(report->note), "%s", loss);
    }
    return QW_STATUS_OK;

failure:
    qw_subtitle_document_dispose(out_document);
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
                       "invalid or unsupported LRC subtitle data");
    }
    return rc;
}

static int subtitle_skip_vtt_block(const SubtitleLine *line)
{
    static const char *const markers[] = { "NOTE", "STYLE", "REGION" };
    size_t i;
    for (i = 0u; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        size_t marker_size = strlen(markers[i]);
        if (line->size >= marker_size &&
            memcmp(line->data, markers[i], marker_size) == 0 &&
            (line->size == marker_size || line->data[marker_size] == ' ' ||
             line->data[marker_size] == '\t')) return 1;
    }
    return 0;
}

int qw_subtitle_parse_text_document(const void *input, size_t input_size,
                                    QwInputFormat input_format,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    int skipped_vtt_metadata = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    SubtitleLine line;
    if (input_format == QW_FORMAT_LRC) {
        return subtitle_parse_lrc_data(input, input_size, out_document, report);
    }
    if (!input || !input_size || !out_document ||
        input_size > SUBTITLE_MAX_INPUT_BYTES ||
        (input_format != QW_FORMAT_SRT && input_format != QW_FORMAT_VTT))
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, input_format);
    if (!subtitle_utf8_valid(data, size)) {
        rc = QW_STATUS_ERR_DECODE;
        goto failure;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    if (input_format == QW_FORMAT_VTT) {
        if (!subtitle_next_line(data, size, &position, &line) ||
            line.size < 6u || memcmp(line.data, "WEBVTT", 6u) != 0 ||
            (line.size > 6u && line.data[6] != ' ' && line.data[6] != '\t')) {
            goto failure;
        }
        while (subtitle_next_line(data, size, &position, &line) &&
               !subtitle_line_blank(&line)) {
            /* The WEBVTT header metadata is intentionally not part of cue data. */
        }
    }
    for (;;) {
        int line_found = 0;
        SubtitleLine timing_line;
        const uint8_t *identifier = NULL;
        size_t identifier_size = 0u;
        const uint8_t *settings = NULL;
        size_t settings_size = 0u;
        uint64_t start_milliseconds;
        uint64_t end_milliseconds;
        QwTextBuffer text = {0};
        while (subtitle_next_line(data, size, &position, &line)) {
            if (!subtitle_line_blank(&line)) {
                line_found = 1;
                break;
            }
        }
        if (!line_found) break;
        if (input_format == QW_FORMAT_VTT && subtitle_skip_vtt_block(&line)) {
            skipped_vtt_metadata = 1;
            while (subtitle_next_line(data, size, &position, &line) &&
                   !subtitle_line_blank(&line)) { }
            if (position >= size) break;
            continue;
        }
        if (subtitle_line_has_arrow(&line)) {
            timing_line = line;
        } else {
            identifier = line.data;
            identifier_size = line.size;
            if (!subtitle_next_line(data, size, &position, &timing_line) ||
                !subtitle_line_has_arrow(&timing_line)) goto failure;
        }
        if (!subtitle_parse_timing_line(&timing_line, input_format,
                &start_milliseconds, &end_milliseconds, &settings,
                &settings_size) ||
            !subtitle_collect_cue_text(data, size, &position, &text)) {
            free(text.data);
            goto failure;
        }
        if (!qw_subtitle_document_add_cue(out_document, start_milliseconds,
                end_milliseconds, 1, identifier, identifier_size, settings,
                settings_size, (const uint8_t *)text.data, text.size)) {
            free(text.data);
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto failure;
        }
        free(text.data);
    }
    if (!qw_subtitle_document_validate(out_document)) goto failure;
    if (report) {
        if (skipped_vtt_metadata) {
            (void)snprintf(report->note, sizeof(report->note),
                "WebVTT cue text and timing were read; NOTE, STYLE, REGION and header metadata are omitted");
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                "%s cues and timing were read as UTF-8; cue text markup is retained as text",
                input_format == QW_FORMAT_VTT ? "WebVTT" : "SRT");
        }
    }
    return QW_STATUS_OK;

failure:
    qw_subtitle_document_dispose(out_document);
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
            "invalid or unsupported %s subtitle data",
            input_format == QW_FORMAT_VTT ? "WebVTT" : "SRT");
    }
    return rc;
}

static int subtitle_open_document(const void *data, size_t size,
                                  QwInputFormat input_format,
                                  const QwConvertOptions *options,
                                  QwSubtitleDocument *out_document,
                                  QwConvertReport *report)
{
    (void)options;
    return qw_subtitle_parse_text_document(data, size, input_format,
                                           out_document, report);
}

static int subtitle_probe_format(const void *data, size_t size,
                                 QwInputFormat input_format)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_text_document(data, size, input_format,
                                             &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int subtitle_srt_probe(const void *data, size_t size)
{
    return subtitle_probe_format(data, size, QW_FORMAT_SRT);
}

static int subtitle_vtt_probe(const void *data, size_t size)
{
    return subtitle_probe_format(data, size, QW_FORMAT_VTT);
}

static int subtitle_lrc_probe(const void *data, size_t size)
{
    return subtitle_probe_format(data, size, QW_FORMAT_LRC);
}

static int subtitle_append_html_escape(QwTextBuffer *html,
                                      const uint8_t *text, size_t size)
{
    size_t i;
    size_t run = 0u;
    for (i = 0u; i < size; ++i) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        if (text[i] == '&') { replacement = "&amp;"; replacement_size = 5u; }
        else if (text[i] == '<') { replacement = "&lt;"; replacement_size = 4u; }
        else if (text[i] == '>') { replacement = "&gt;"; replacement_size = 4u; }
        else if (text[i] == '"') { replacement = "&quot;"; replacement_size = 6u; }
        else if (text[i] == '\n') { replacement = "<br/>"; replacement_size = 5u; }
        else if (text[i] == '\t') { replacement = "&#9;"; replacement_size = 4u; }
        if (!replacement) continue;
        if (i > run && !qw_text_buffer_append(html, (const char *)text + run,
                                               i - run)) return 0;
        if (!qw_text_buffer_append(html, replacement, replacement_size)) return 0;
        run = i + 1u;
    }
    return run == size || qw_text_buffer_append(html, (const char *)text + run,
                                                 size - run);
}

static int subtitle_append_time(QwTextBuffer *buffer, uint64_t milliseconds,
                                char separator)
{
    uint64_t hours = milliseconds / 3600000u;
    unsigned minutes = (unsigned)((milliseconds / 60000u) % 60u);
    unsigned seconds = (unsigned)((milliseconds / 1000u) % 60u);
    unsigned fraction = (unsigned)(milliseconds % 1000u);
    char time_text[64];
    int written = snprintf(time_text, sizeof(time_text),
        "%02llu:%02u:%02u%c%03u", (unsigned long long)hours, minutes,
        seconds, separator, fraction);
    return written > 0 && (size_t)written < sizeof(time_text) &&
           qw_text_buffer_append(buffer, time_text, (size_t)written);
}

static int subtitle_append_lrc_time(QwTextBuffer *buffer,
                                    uint64_t milliseconds)
{
    uint64_t minutes = milliseconds / 60000u;
    unsigned seconds = (unsigned)((milliseconds / 1000u) % 60u);
    unsigned centiseconds = (unsigned)((milliseconds % 1000u) / 10u);
    char time_text[64];
    int written = snprintf(time_text, sizeof(time_text), "[%02llu:%02u.%02u]",
        (unsigned long long)minutes, seconds, centiseconds);
    return written > 0 && (size_t)written < sizeof(time_text) &&
           qw_text_buffer_append(buffer, time_text, (size_t)written);
}

static int subtitle_append_lrc_text(QwTextBuffer *buffer,
                                    const uint8_t *text, size_t size,
                                    int *line_breaks_flattened)
{
    size_t run_start = 0u;
    size_t i;
    for (i = 0u; i < size; ++i) {
        if (text[i] != '\n' && text[i] != '\t') continue;
        if (i > run_start &&
            !qw_text_buffer_append(buffer, (const char *)text + run_start,
                                   i - run_start)) return 0;
        if (!qw_text_buffer_append(buffer, " ", 1u)) return 0;
        run_start = i + 1u;
        if (line_breaks_flattened) *line_breaks_flattened = 1;
    }
    return run_start == size ||
           qw_text_buffer_append(buffer, (const char *)text + run_start,
                                 size - run_start);
}

int qw_subtitle_document_render_layout(const QwSubtitleDocument *subtitle,
                                      QwLayoutDocument *out_document,
                                      QwConvertReport *report)
{
    QwTextBuffer html = {0};
    size_t i;
    int rc = QW_STATUS_OK;
    if (!out_document || !qw_subtitle_document_validate(subtitle))
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    html.limit = (size_t)SUBTITLE_MAX_OUTPUT_BYTES + 1u;
    if (!SUBTITLE_APPEND_LITERAL(&html,
        "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>")) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (i = 0u; i < subtitle->cue_count; ++i) {
        const QwSubtitleCue *cue = &subtitle->cues[i];
        if (!SUBTITLE_APPEND_LITERAL(&html,
                "<section class=\"subtitle-cue\"><h2>Cue ") ||
            !subtitle_append_time(&html, cue->start_milliseconds, '.') ||
            !SUBTITLE_APPEND_LITERAL(&html, " – ") ||
            !subtitle_append_time(&html, cue->end_milliseconds, '.') ||
            !SUBTITLE_APPEND_LITERAL(&html, "</h2><p class=\"caption\">") ||
            !subtitle_append_html_escape(&html, cue->text_utf8, cue->text_size) ||
            !SUBTITLE_APPEND_LITERAL(&html, "</p></section>")) {
            rc = html.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    if (!SUBTITLE_APPEND_LITERAL(&html, "</body></html>")) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto cleanup;
    }
    out_document->html_utf8 = (const uint8_t *)html.data;
    out_document->html_size = html.size;
    out_document->context = html.data;
    out_document->dispose = free;
    html.data = NULL;
    if (report) {
        if (subtitle->input_format == QW_FORMAT_LRC) {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu subtitle cues rendered with five-second inferred timing; cue styles and WebVTT settings are not laid out",
                subtitle->cue_count);
        } else if (subtitle->input_format == QW_FORMAT_SUB) {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu MicroDVD cues rendered with frame timing; style metadata is not laid out",
                subtitle->cue_count);
        } else if (subtitle->input_format == QW_FORMAT_TTML) {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu TTML cues rendered; styles, regions and nested span timing are not laid out",
                subtitle->cue_count);
        } else if (subtitle->input_format == QW_FORMAT_ASS ||
                   subtitle->input_format == QW_FORMAT_SSA) {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu ASS/SSA cues rendered; script styles, override tags and positioning are not laid out",
                subtitle->cue_count);
        } else if (subtitle->input_format == QW_FORMAT_SAMI) {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu SAMI cues rendered; HTML styles and alternate caption classes are not laid out",
                subtitle->cue_count);
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                "%zu subtitle cues rendered with timing; cue styles and WebVTT settings are not laid out",
                subtitle->cue_count);
        }
    }
    rc = QW_STATUS_OK;

cleanup:
    free(html.data);
    return rc;
}

static int subtitle_open_layout(const void *data, size_t size,
                                QwInputFormat input_format,
                                const QwConvertOptions *options,
                                QwLayoutDocument *out_document,
                                QwConvertReport *report)
{
    QwSubtitleDocument subtitle;
    int rc;
    (void)options;
    if (!out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    rc = qw_subtitle_parse_text_document(data, size, input_format,
                                         &subtitle, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_subtitle_document_render_layout(&subtitle, out_document, report);
    qw_subtitle_document_dispose(&subtitle);
    return rc;
}

static int subtitle_write_document(const QwSubtitleDocument *document,
                                   QwOutputFormat output_format,
                                   const QwConvertOptions *options,
                                   uint8_t **out_data, size_t *out_size,
                                   QwConvertReport *report)
{
    QwTextBuffer output = {0};
    size_t i;
    int settings_dropped = 0;
    int metadata_dropped = 0;
    int explicit_ends_dropped = 0;
    int inferred_ends_written = 0;
    int precision_reduced = 0;
    int line_breaks_flattened = 0;
    int rc = QW_STATUS_OK;
    (void)options;
    if (!document || !out_data || !out_size ||
        (output_format != QW_FORMAT_SRT && output_format != QW_FORMAT_VTT &&
         output_format != QW_FORMAT_LRC) ||
        !qw_subtitle_document_validate(document)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    output.limit = (size_t)SUBTITLE_MAX_OUTPUT_BYTES + 1u;
    if (output_format == QW_FORMAT_VTT &&
        !qw_text_buffer_append(&output, "WEBVTT\n\n", 8u)) {
        rc = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (i = 0u; i < document->cue_count; ++i) {
        const QwSubtitleCue *cue = &document->cues[i];
        char sequence[32];
        const uint8_t *identifier = cue->identifier_utf8;
        size_t identifier_size = cue->identifier_size;
        if (output_format == QW_FORMAT_LRC) {
            if (subtitle_lrc_text_starts_timestamp(cue->text_utf8,
                                                    cue->text_size)) {
                rc = QW_STATUS_ERR_UNSUPPORTED;
                if (report) {
                    (void)snprintf(report->note, sizeof(report->note),
                        "LRC cannot represent lyric text beginning with a numeric timestamp tag");
                }
                goto cleanup;
            }
            if (cue->identifier_size || cue->settings_size) metadata_dropped = 1;
            if (cue->end_time_explicit) explicit_ends_dropped = 1;
            if (cue->start_milliseconds % 10u) precision_reduced = 1;
            if (!subtitle_append_lrc_time(&output, cue->start_milliseconds)) {
                rc = output.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                    ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            if (!subtitle_append_lrc_text(&output, cue->text_utf8,
                                          cue->text_size,
                                          &line_breaks_flattened)) {
                rc = output.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                    ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            if (!qw_text_buffer_append(&output, "\n", 1u)) {
                rc = output.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                    ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            if (output.size > (size_t)SUBTITLE_MAX_OUTPUT_BYTES) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
            continue;
        }
        if (!cue->end_time_explicit) inferred_ends_written = 1;
        if (output_format == QW_FORMAT_SRT && cue->settings_size)
            settings_dropped = 1;
        if (output_format == QW_FORMAT_SRT && !identifier_size) {
            int written = snprintf(sequence, sizeof(sequence), "%zu", i + 1u);
            if (written < 0 || (size_t)written >= sizeof(sequence)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            identifier = (const uint8_t *)sequence;
            identifier_size = (size_t)written;
        }
        if (identifier_size &&
            (!qw_text_buffer_append(&output, (const char *)identifier,
                                    identifier_size) ||
             !qw_text_buffer_append(&output, "\n", 1u))) {
            rc = output.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (!subtitle_append_time(&output, cue->start_milliseconds,
                output_format == QW_FORMAT_SRT ? ',' : '.') ||
            !qw_text_buffer_append(&output, " --> ", 5u) ||
            !subtitle_append_time(&output, cue->end_milliseconds,
                output_format == QW_FORMAT_SRT ? ',' : '.') ||
            (output_format == QW_FORMAT_VTT && cue->settings_size &&
             (!qw_text_buffer_append(&output, " ", 1u) ||
              !qw_text_buffer_append(&output,
                  (const char *)cue->settings_utf8, cue->settings_size))) ||
            !qw_text_buffer_append(&output, "\n", 1u) ||
            !qw_text_buffer_append(&output, (const char *)cue->text_utf8,
                                   cue->text_size) ||
            !qw_text_buffer_append(&output, "\n\n", 2u)) {
            rc = output.size >= (size_t)SUBTITLE_MAX_OUTPUT_BYTES
                ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        if (output.size > (size_t)SUBTITLE_MAX_OUTPUT_BYTES) {
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    if (report) {
        if (output_format == QW_FORMAT_LRC) {
            (void)snprintf(report->note, sizeof(report->note),
                "LRC output keeps cue start times and lyric text%s%s%s%s",
                explicit_ends_dropped ? "; explicit cue ends are dropped" : "",
                metadata_dropped ? "; cue identifiers and settings are dropped" : "",
                precision_reduced ? "; start times are truncated to centiseconds" : "",
                line_breaks_flattened ? "; line breaks and tabs become spaces" : "");
        } else if (inferred_ends_written) {
            (void)snprintf(report->note, sizeof(report->note),
                "%s output uses five-second inferred ends for LRC cues%s",
                output_format == QW_FORMAT_VTT ? "WebVTT" : "SRT",
                settings_dropped ? "; WebVTT cue settings are dropped" : "");
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                settings_dropped
                    ? "SRT output keeps cue timing and text but drops WebVTT cue settings"
                    : "%s output keeps cue timing, identifiers and UTF-8 text",
                output_format == QW_FORMAT_VTT ? "WebVTT" : "SRT");
        }
    }
    return QW_STATUS_OK;

cleanup:
    free(output.data);
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
            "subtitle output failed with status %d", rc);
    }
    return rc;
}

const QwFormatDescriptor qw_format_srt = {
    .id = QW_FORMAT_SRT,
    .name = "SubRip subtitles",
    .probe = subtitle_srt_probe,
    .open_layout = subtitle_open_layout,
    .max_input_bytes = SUBTITLE_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = subtitle_open_document
};

const QwFormatDescriptor qw_format_vtt = {
    .id = QW_FORMAT_VTT,
    .name = "WebVTT subtitles",
    .probe = subtitle_vtt_probe,
    .open_layout = subtitle_open_layout,
    .max_input_bytes = SUBTITLE_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = subtitle_open_document
};

const QwFormatDescriptor qw_format_lrc = {
    .id = QW_FORMAT_LRC,
    .name = "LRC lyrics",
    .probe = subtitle_lrc_probe,
    .open_layout = subtitle_open_layout,
    .max_input_bytes = SUBTITLE_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = subtitle_open_document
};

const QwSubtitleWriterDescriptor qw_writer_srt_subtitle = {
    QW_FORMAT_SRT,
    "SRT timed-cue writer",
    subtitle_write_document,
    SUBTITLE_MAX_OUTPUT_BYTES
};

const QwSubtitleWriterDescriptor qw_writer_vtt_subtitle = {
    QW_FORMAT_VTT,
    "WebVTT timed-cue writer",
    subtitle_write_document,
    SUBTITLE_MAX_OUTPUT_BYTES
};

const QwSubtitleWriterDescriptor qw_writer_lrc_subtitle = {
    QW_FORMAT_LRC,
    "LRC timed-lyric writer",
    subtitle_write_document,
    SUBTITLE_MAX_OUTPUT_BYTES
};
