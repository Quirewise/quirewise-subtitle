#include "subtitle_format.h"

#include "../../core/text_buffer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ASS_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    ASS_MAX_OUTPUT_BYTES = 64u * 1024u * 1024u,
    ASS_MAX_EVENT_FIELDS = 32u
};

typedef struct AssSlice {
    const uint8_t *data;
    size_t size;
} AssSlice;

typedef struct AssEventFormat {
    size_t field_count;
    size_t start_index;
    size_t end_index;
    size_t text_index;
    int has_start;
    int has_end;
    int has_text;
} AssEventFormat;

typedef struct AssLine {
    const uint8_t *data;
    size_t size;
} AssLine;

static uint8_t ass_ascii_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int ass_equal_ascii(const uint8_t *left, size_t left_size,
                           const char *right)
{
    size_t i;
    size_t right_size;
    if (!left || !right) return 0;
    right_size = strlen(right);
    if (left_size != right_size) return 0;
    for (i = 0u; i < left_size; ++i) {
        if (ass_ascii_lower(left[i]) != ass_ascii_lower((uint8_t)right[i]))
            return 0;
    }
    return 1;
}

static int ass_starts_ascii(const uint8_t *data, size_t size,
                            const char *prefix, size_t *out_prefix_size)
{
    size_t prefix_size;
    if (!data || !prefix) return 0;
    prefix_size = strlen(prefix);
    if (size < prefix_size ||
        !ass_equal_ascii(data, prefix_size, prefix)) return 0;
    if (out_prefix_size) *out_prefix_size = prefix_size;
    return 1;
}

static void ass_trim_slice(AssSlice *slice)
{
    while (slice->size && (slice->data[0] == ' ' || slice->data[0] == '\t')) {
        ++slice->data;
        --slice->size;
    }
    while (slice->size &&
           (slice->data[slice->size - 1u] == ' ' ||
            slice->data[slice->size - 1u] == '\t')) --slice->size;
}

static int ass_utf8_valid(const uint8_t *data, size_t size)
{
    size_t position = 0u;
    if (!data && size) return 0;
    while (position < size) {
        uint8_t first = data[position++];
        size_t continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t i;
        if (first < 0x80u) {
            if (!first || (first < 0x20u && first != '\r' && first != '\n' &&
                           first != '\t')) return 0;
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
            uint8_t next = data[position++];
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

static int ass_next_line(const uint8_t *data, size_t size, size_t *position,
                         AssLine *line)
{
    size_t start;
    if (!data || !position || !line || *position >= size) return 0;
    start = *position;
    while (*position < size && data[*position] != '\n' &&
           data[*position] != '\r') ++*position;
    line->data = data + start;
    line->size = *position - start;
    if (*position < size && data[*position] == '\r') ++*position;
    if (*position < size && data[*position] == '\n') ++*position;
    return 1;
}

static int ass_read_uint(const uint8_t **cursor, const uint8_t *end,
                         uint64_t *out_value)
{
    uint64_t value = 0u;
    const uint8_t *start;
    if (!cursor || !*cursor || !end || !out_value) return 0;
    start = *cursor;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        unsigned digit = (unsigned)(**cursor - '0');
        if (value > (UINT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++*cursor;
    }
    if (*cursor == start) return 0;
    *out_value = value;
    return 1;
}

static int ass_mul_u64(uint64_t left, uint64_t right, uint64_t *out_value)
{
    if (!out_value || (right && left > UINT64_MAX / right)) return 0;
    *out_value = left * right;
    return 1;
}

static int ass_add_u64(uint64_t left, uint64_t right, uint64_t *out_value)
{
    if (!out_value || left > UINT64_MAX - right) return 0;
    *out_value = left + right;
    return 1;
}

static int ass_parse_time(const AssSlice *slice, uint64_t *out_milliseconds)
{
    const uint8_t *cursor;
    const uint8_t *end;
    uint64_t hours;
    uint64_t minutes;
    uint64_t seconds;
    uint64_t centiseconds;
    uint64_t hours_ms;
    uint64_t minutes_ms;
    uint64_t seconds_ms;
    uint64_t total;
    AssSlice value;
    if (!slice || !out_milliseconds) return 0;
    value = *slice;
    ass_trim_slice(&value);
    cursor = value.data;
    end = value.data + value.size;
    if (!ass_read_uint(&cursor, end, &hours) || cursor >= end || *cursor++ != ':' ||
        !ass_read_uint(&cursor, end, &minutes) || minutes >= 60u ||
        cursor >= end || *cursor++ != ':' ||
        !ass_read_uint(&cursor, end, &seconds) || seconds >= 60u ||
        cursor >= end || *cursor++ != '.' || end - cursor != 2 ||
        !ass_read_uint(&cursor, end, &centiseconds) || centiseconds >= 100u ||
        cursor != end ||
        !ass_mul_u64(hours, 3600000u, &hours_ms) ||
        !ass_mul_u64(minutes, 60000u, &minutes_ms) ||
        !ass_mul_u64(seconds, 1000u, &seconds_ms) ||
        !ass_add_u64(hours_ms, minutes_ms, &total) ||
        !ass_add_u64(total, seconds_ms, &total) ||
        !ass_add_u64(total, centiseconds * 10u, out_milliseconds)) return 0;
    return 1;
}

static int ass_parse_format(const uint8_t *data, size_t size,
                            AssEventFormat *format)
{
    size_t position = 0u;
    AssEventFormat parsed;
    if (!data || !format) return 0;
    memset(&parsed, 0, sizeof(parsed));
    while (position <= size) {
        size_t end = position;
        AssSlice field;
        while (end < size && data[end] != ',') ++end;
        field.data = data + position;
        field.size = end - position;
        ass_trim_slice(&field);
        if (!field.size || parsed.field_count >= ASS_MAX_EVENT_FIELDS) return 0;
        if (ass_equal_ascii(field.data, field.size, "start")) {
            if (parsed.has_start) return 0;
            parsed.start_index = parsed.field_count;
            parsed.has_start = 1;
        } else if (ass_equal_ascii(field.data, field.size, "end")) {
            if (parsed.has_end) return 0;
            parsed.end_index = parsed.field_count;
            parsed.has_end = 1;
        } else if (ass_equal_ascii(field.data, field.size, "text")) {
            if (parsed.has_text) return 0;
            parsed.text_index = parsed.field_count;
            parsed.has_text = 1;
        }
        ++parsed.field_count;
        if (end == size) break;
        position = end + 1u;
    }
    if (!parsed.has_start || !parsed.has_end || !parsed.has_text ||
        parsed.text_index + 1u != parsed.field_count) return 0;
    *format = parsed;
    return 1;
}

static int ass_split_event_fields(const uint8_t *data, size_t size,
                                  const AssEventFormat *format,
                                  AssSlice *fields)
{
    size_t position = 0u;
    size_t i;
    if (!format || !fields || !format->field_count ||
        format->field_count > ASS_MAX_EVENT_FIELDS) return 0;
    while (position < size && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    for (i = 0u; i < format->field_count; ++i) {
        size_t end = position;
        if (i + 1u < format->field_count) {
            while (end < size && data[end] != ',') ++end;
            if (end >= size) return 0;
        } else {
            end = size;
        }
        fields[i].data = data + position;
        fields[i].size = end - position;
        if (i + 1u < format->field_count) position = end + 1u;
        else position = end;
    }
    return position == size;
}

static int ass_drawing_override(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0u; i + 2u < size; ++i) {
        size_t j;
        uint64_t value = 0u;
        if (data[i] != '\\' || ass_ascii_lower(data[i + 1u]) != 'p' ||
            data[i + 2u] < '0' || data[i + 2u] > '9') continue;
        j = i + 2u;
        while (j < size && data[j] >= '0' && data[j] <= '9') {
            unsigned digit = (unsigned)(data[j] - '0');
            if (value > (UINT64_MAX - digit) / 10u) return 1;
            value = value * 10u + digit;
            ++j;
        }
        if (value) return 1;
    }
    return 0;
}

static int ass_append(QwTextBuffer *buffer, const void *data, size_t size,
                      int *out_error)
{
    if (!buffer || !out_error || (!data && size)) return 0;
    if (buffer->size > ASS_MAX_OUTPUT_BYTES ||
        size > ASS_MAX_OUTPUT_BYTES - buffer->size) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    buffer->limit = (size_t)ASS_MAX_OUTPUT_BYTES + 1u;
    if (!qw_text_buffer_append(buffer, (const char *)data, size)) {
        *out_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int ass_decode_text(const uint8_t *data, size_t size, unsigned wrap_style,
                           QwTextBuffer *text, int *out_error)
{
    static const char nbsp[] = "\xc2\xa0";
    size_t i = 0u;
    while (i < size) {
        if (data[i] == '{' && i + 1u < size && data[i + 1u] == '\\') {
            size_t close = i + 2u;
            while (close < size && data[close] != '}') ++close;
            if (close < size) {
                if (ass_drawing_override(data + i + 2u, close - (i + 2u))) {
                    *out_error = QW_STATUS_ERR_UNSUPPORTED;
                    return 0;
                }
                i = close + 1u;
                continue;
            }
        }
        if (data[i] == '\\' && i + 1u < size) {
            switch (data[i + 1u]) {
            case 'N':
                if (!ass_append(text, "\n", 1u, out_error)) return 0;
                i += 2u;
                continue;
            case 'n':
                if (wrap_style == 2u) {
                    if (!ass_append(text, "\n", 1u, out_error)) return 0;
                } else if (!ass_append(text, " ", 1u, out_error)) return 0;
                i += 2u;
                continue;
            case 'h':
                if (!ass_append(text, nbsp, sizeof(nbsp) - 1u, out_error))
                    return 0;
                i += 2u;
                continue;
            case '{':
            case '}':
                if (!ass_append(text, data + i + 1u, 1u, out_error)) return 0;
                i += 2u;
                continue;
            default:
                break;
            }
        }
        if (!ass_append(text, data + i, 1u, out_error)) return 0;
        ++i;
    }
    return 1;
}

static int ass_parse_dialogue(const uint8_t *data, size_t size,
                              const AssEventFormat *format,
                              unsigned wrap_style,
                              QwSubtitleDocument *document,
                              int *out_error, int *out_empty)
{
    AssSlice fields[ASS_MAX_EVENT_FIELDS];
    QwTextBuffer text = {0};
    uint64_t start_ms;
    uint64_t end_ms;
    int error = QW_STATUS_OK;
    int added;
    if (!ass_split_event_fields(data, size, format, fields) ||
        !ass_parse_time(&fields[format->start_index], &start_ms) ||
        !ass_parse_time(&fields[format->end_index], &end_ms) ||
        start_ms >= end_ms) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!ass_decode_text(fields[format->text_index].data,
                         fields[format->text_index].size,
                         wrap_style, &text, &error)) {
        free(text.data);
        *out_error = error ? error : QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!text.size) {
        ++*out_empty;
        free(text.data);
        return 1;
    }
    added = qw_subtitle_document_add_cue(document, start_ms, end_ms, 1,
                NULL, 0u, NULL, 0u, (const uint8_t *)text.data, text.size);
    free(text.data);
    if (!added) {
        *out_error = document->cue_count >= QW_SUBTITLE_MAX_CUES
            ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

int qw_subtitle_parse_ass_document(const void *input, size_t input_size,
                                   QwInputFormat input_format,
                                   QwSubtitleDocument *out_document,
                                   QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    int in_events = 0;
    int saw_v4_styles = 0;
    int saw_v4plus_styles = 0;
    int script_v4 = 0;
    int script_v4plus = 0;
    int saw_dialogue = 0;
    int empty_dialogues = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    unsigned wrap_style = 0u;
    AssEventFormat event_format;
    AssLine line;
    if (!input || !out_document ||
        (input_format != QW_FORMAT_ASS && input_format != QW_FORMAT_SSA))
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, input_format);
    if (!input_size) goto failure;
    if (input_size > ASS_MAX_INPUT_BYTES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto failure;
    }
    if (!ass_utf8_valid(data, size)) {
        rc = QW_STATUS_ERR_DECODE;
        goto failure;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu &&
        data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    memset(&event_format, 0, sizeof(event_format));
    while (ass_next_line(data, size, &position, &line)) {
        AssSlice raw;
        AssSlice trimmed;
        size_t prefix_size;
        raw.data = line.data;
        raw.size = line.size;
        while (raw.size && (raw.data[0] == ' ' || raw.data[0] == '\t')) {
            ++raw.data;
            --raw.size;
        }
        trimmed = raw;
        ass_trim_slice(&trimmed);
        if (!trimmed.size) continue;
        if (trimmed.size >= 2u && trimmed.data[0] == '[' &&
            trimmed.data[trimmed.size - 1u] == ']') {
            size_t section_size = trimmed.size - 2u;
            const uint8_t *section = trimmed.data + 1u;
            in_events = ass_equal_ascii(section, section_size, "Events");
            if (ass_equal_ascii(section, section_size, "V4 Styles"))
                saw_v4_styles = 1;
            if (ass_equal_ascii(section, section_size, "V4+ Styles"))
                saw_v4plus_styles = 1;
            continue;
        }
        if (!in_events) {
            if (ass_starts_ascii(trimmed.data, trimmed.size, "ScriptType:",
                                 &prefix_size)) {
                AssSlice value = {trimmed.data + prefix_size,
                                  trimmed.size - prefix_size};
                ass_trim_slice(&value);
                if (ass_equal_ascii(value.data, value.size, "v4.00"))
                    script_v4 = 1;
                else if (ass_equal_ascii(value.data, value.size, "v4.00+"))
                    script_v4plus = 1;
            } else if (ass_starts_ascii(trimmed.data, trimmed.size, "WrapStyle:",
                                        &prefix_size)) {
                AssSlice value = {trimmed.data + prefix_size,
                                  trimmed.size - prefix_size};
                const uint8_t *cursor;
                const uint8_t *end;
                uint64_t parsed;
                ass_trim_slice(&value);
                cursor = value.data;
                end = value.data + value.size;
                if (ass_read_uint(&cursor, end, &parsed) && cursor == end &&
                    parsed <= 3u) wrap_style = (unsigned)parsed;
            }
            continue;
        }
        if (ass_starts_ascii(trimmed.data, trimmed.size, "Format:",
                             &prefix_size)) {
            AssSlice value = {trimmed.data + prefix_size,
                              trimmed.size - prefix_size};
            ass_trim_slice(&value);
            if (!ass_parse_format(value.data, value.size, &event_format))
                goto failure;
            continue;
        }
        if (ass_starts_ascii(trimmed.data, trimmed.size, "Dialogue:",
                             &prefix_size)) {
            if (!event_format.field_count ||
                !ass_parse_dialogue(raw.data + prefix_size,
                    raw.size - prefix_size, &event_format, wrap_style,
                    out_document, &rc, &empty_dialogues)) goto failure;
            saw_dialogue = 1;
        }
    }
    if (!saw_dialogue || !out_document->cue_count ||
        !qw_subtitle_document_validate(out_document)) goto failure;
    if (input_format == QW_FORMAT_ASS) {
        if (!(script_v4plus || saw_v4plus_styles) ||
            (script_v4 && !script_v4plus && !saw_v4plus_styles)) goto failure;
    } else if (!(script_v4 || saw_v4_styles) ||
               (script_v4plus && !script_v4 && !saw_v4_styles)) {
        goto failure;
    }
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "%s dialogue times and plain text read; styles, positioning, names, margins, layers and effects are flattened%s",
            input_format == QW_FORMAT_ASS ? "ASS" : "SSA",
            empty_dialogues ? "; empty dialogue events are omitted" : "");
    }
    return QW_STATUS_OK;

failure:
    qw_subtitle_document_dispose(out_document);
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
                       "invalid or unsupported %s subtitle data",
                       input_format == QW_FORMAT_ASS ? "ASS" : "SSA");
    }
    return rc;
}

static int ass_open_subtitle(const void *data, size_t size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwSubtitleDocument *out_document,
                             QwConvertReport *report)
{
    (void)options;
    return qw_subtitle_parse_ass_document(data, size, input_format,
                                          out_document, report);
}

static int ass_probe(const void *data, size_t size)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_ass_document(data, size, QW_FORMAT_ASS,
                                            &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int ssa_probe(const void *data, size_t size)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_ass_document(data, size, QW_FORMAT_SSA,
                                            &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int ass_open_layout(const void *data, size_t size,
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
    rc = qw_subtitle_parse_ass_document(data, size, input_format,
                                        &subtitle, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_subtitle_document_render_layout(&subtitle, out_document, report);
    qw_subtitle_document_dispose(&subtitle);
    return rc;
}

static int ass_append_time(QwTextBuffer *output, uint64_t milliseconds,
                           int *out_error, int *out_precision_lost)
{
    char value[64];
    uint64_t centiseconds = milliseconds / 10u;
    uint64_t hours;
    uint64_t minutes;
    uint64_t seconds;
    uint64_t hundredths;
    int length;
    if (milliseconds % 10u >= 5u) ++centiseconds;
    hours = centiseconds / 360000u;
    minutes = (centiseconds / 6000u) % 60u;
    seconds = (centiseconds / 100u) % 60u;
    hundredths = centiseconds % 100u;
    length = snprintf(value, sizeof(value), "%" PRIu64 ":%02" PRIu64
        ":%02" PRIu64 ".%02" PRIu64, hours, minutes, seconds, hundredths);
    if (length < 0 || (size_t)length >= sizeof(value)) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (milliseconds % 10u && out_precision_lost) *out_precision_lost = 1;
    return ass_append(output, value, (size_t)length, out_error);
}

static int ass_append_text(QwTextBuffer *output, const uint8_t *data,
                           size_t size, int *out_error)
{
    static const char line_break[] = "\\N";
    static const char escaped_open[] = "\\{";
    static const char escaped_close[] = "\\}";
    size_t i = 0u;
    while (i < size) {
        if (data[i] == '\r' || data[i] == '\n') {
            if (data[i] == '\r' && i + 1u < size && data[i + 1u] == '\n')
                ++i;
            if (!ass_append(output, line_break, sizeof(line_break) - 1u,
                            out_error)) return 0;
        } else if (data[i] == '{') {
            if (!ass_append(output, escaped_open, sizeof(escaped_open) - 1u,
                            out_error)) return 0;
        } else if (data[i] == '}') {
            if (!ass_append(output, escaped_close, sizeof(escaped_close) - 1u,
                            out_error)) return 0;
        } else if (data[i] == '\\' && i + 1u < size &&
                   (data[i + 1u] == 'N' || data[i + 1u] == 'n' ||
                    data[i + 1u] == 'h' || data[i + 1u] == '{' ||
                    data[i + 1u] == '}')) {
            *out_error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        } else if (!ass_append(output, data + i, 1u, out_error)) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int ass_write_document(const QwSubtitleDocument *document,
                              QwOutputFormat output_format,
                              const QwConvertOptions *options,
                              uint8_t **out_data, size_t *out_size,
                              QwConvertReport *report)
{
    static const char ass_header[] =
        "[Script Info]\nScriptType: v4.00+\nWrapStyle: 0\n"
        "ScaledBorderAndShadow: yes\n\n[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
        "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,"
        "&H80000000,-1,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
        "MarginV, Effect, Text\n";
    static const char ssa_header[] =
        "[Script Info]\nScriptType: v4.00\nWrapStyle: 0\n\n"
        "[V4 Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "TertiaryColour, BackColour, Bold, Italic, BorderStyle, Outline, "
        "Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
        "Style: Default,Arial,20,&H00FFFFFF,&H0000FFFF,&H00000000,"
        "&H80000000,0,0,1,2,0,2,10,10,10,0,1\n\n[Events]\n"
        "Format: Marked, Start, End, Style, Name, MarginL, MarginR, "
        "MarginV, Effect, Text\n";
    static const char ass_event_prefix[] = "Dialogue: 0,";
    static const char ssa_event_prefix[] = "Dialogue: Marked=0,";
    static const char event_fields[] = ",Default,,0,0,0,,";
    QwTextBuffer output = {0};
    int error = QW_STATUS_OK;
    int precision_lost = 0;
    int metadata_dropped = 0;
    int inferred_end_written = 0;
    int literal_ass_escape_rejected = 0;
    size_t i;
    (void)options;
    if (!document || !out_data || !out_size ||
        (output_format != QW_FORMAT_ASS && output_format != QW_FORMAT_SSA) ||
        !qw_subtitle_document_validate(document)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    output.limit = (size_t)ASS_MAX_OUTPUT_BYTES + 1u;
    if (output_format == QW_FORMAT_ASS) {
        if (!ass_append(&output, ass_header, sizeof(ass_header) - 1u, &error))
            goto cleanup;
    } else if (!ass_append(&output, ssa_header, sizeof(ssa_header) - 1u,
                           &error)) goto cleanup;
    for (i = 0u; i < document->cue_count; ++i) {
        const QwSubtitleCue *cue = &document->cues[i];
        const char *prefix = output_format == QW_FORMAT_ASS
            ? ass_event_prefix : ssa_event_prefix;
        size_t prefix_size = output_format == QW_FORMAT_ASS
            ? sizeof(ass_event_prefix) - 1u : sizeof(ssa_event_prefix) - 1u;
        if (!ass_append(&output, prefix, prefix_size, &error) ||
            !ass_append_time(&output, cue->start_milliseconds, &error,
                             &precision_lost) ||
            !ass_append(&output, ",", 1u, &error) ||
            !ass_append_time(&output, cue->end_milliseconds, &error,
                             &precision_lost) ||
            !ass_append(&output, event_fields, sizeof(event_fields) - 1u,
                        &error)) goto cleanup;
        if (!ass_append_text(&output, cue->text_utf8, cue->text_size, &error)) {
            if (error == QW_STATUS_ERR_UNSUPPORTED) literal_ass_escape_rejected = 1;
            goto cleanup;
        }
        if (!ass_append(&output, "\n", 1u, &error)) goto cleanup;
        if (cue->identifier_size || cue->settings_size) metadata_dropped = 1;
        if (!cue->end_time_explicit) inferred_end_written = 1;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    output.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "%s output keeps cue timing and plain text; styles, positioning, names, margins, layers and effects are omitted%s%s%s",
            output_format == QW_FORMAT_ASS ? "ASS" : "SSA",
            metadata_dropped ? "; source cue identifiers and settings are omitted" : "",
            precision_lost ? "; timestamps rounded to centiseconds" : "",
            inferred_end_written ? "; inferred cue ends are written as explicit times" : "");
    }
    error = QW_STATUS_OK;

cleanup:
    free(output.data);
    if (literal_ass_escape_rejected && report) {
        (void)snprintf(report->note, sizeof(report->note),
            "ASS/SSA cannot safely represent literal backslashes followed by N, n, h, { or } in cue text");
    }
    return error;
}

const QwFormatDescriptor qw_format_ass = {
    .id = QW_FORMAT_ASS,
    .name = "Advanced SubStation Alpha subtitles",
    .probe = ass_probe,
    .open_layout = ass_open_layout,
    .max_input_bytes = ASS_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = ass_open_subtitle
};

const QwFormatDescriptor qw_format_ssa = {
    .id = QW_FORMAT_SSA,
    .name = "SubStation Alpha subtitles",
    .probe = ssa_probe,
    .open_layout = ass_open_layout,
    .max_input_bytes = ASS_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = ass_open_subtitle
};

const QwSubtitleWriterDescriptor qw_writer_ass_subtitle = {
    QW_FORMAT_ASS,
    "ASS timed-cue writer",
    ass_write_document,
    ASS_MAX_OUTPUT_BYTES
};

const QwSubtitleWriterDescriptor qw_writer_ssa_subtitle = {
    QW_FORMAT_SSA,
    "SSA timed-cue writer",
    ass_write_document,
    ASS_MAX_OUTPUT_BYTES
};
