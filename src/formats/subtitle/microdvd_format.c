#include "subtitle_format.h"

#include "../../core/text_buffer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MICRODVD_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    MICRODVD_MAX_OUTPUT_BYTES = 64u * 1024u * 1024u,
    MICRODVD_MAX_FPS_DENOMINATOR = 1000000u,
    MICRODVD_FPS_DECIMAL_PLACES = 6u
};

typedef struct MicrodvdLine {
    const uint8_t *data;
    size_t size;
} MicrodvdLine;

static int microdvd_utf8_valid(const uint8_t *data, size_t size)
{
    size_t position = 0u;
    while (position < size) {
        uint8_t first = data[position++];
        size_t continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t i;
        if (first < 0x80u) {
            if (first == 0u || (first < 0x20u && first != '\n' &&
                first != '\r' && first != '\t')) return 0;
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
            (codepoint >= 0xd800u && codepoint <= 0xdfffu)) return 0;
    }
    return 1;
}

static int microdvd_next_line(const uint8_t *data, size_t size,
                              size_t *position, MicrodvdLine *out_line)
{
    size_t start;
    if (!data || !position || !out_line || *position >= size) return 0;
    start = *position;
    while (*position < size && data[*position] != '\n' &&
           data[*position] != '\r') ++*position;
    out_line->data = data + start;
    out_line->size = *position - start;
    if (*position < size && data[*position] == '\r') {
        ++*position;
        if (*position < size && data[*position] == '\n') ++*position;
    } else if (*position < size) {
        ++*position;
    }
    return 1;
}

static int microdvd_line_blank(const MicrodvdLine *line)
{
    size_t i;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static int microdvd_default_style_line(const MicrodvdLine *line)
{
    static const char prefix[] = "{DEFAULT}{";
    return line->size >= sizeof(prefix) - 1u &&
           memcmp(line->data, prefix, sizeof(prefix) - 1u) == 0;
}

static int microdvd_read_uint(const uint8_t **cursor, const uint8_t *end,
                              uint64_t *out_value)
{
    uint64_t value = 0u;
    int has_digit = 0;
    while (*cursor < end && **cursor >= '0' && **cursor <= '9') {
        uint8_t digit = *(*cursor)++ - '0';
        if (value > (UINT64_MAX - digit) / 10u) return 0;
        value = value * 10u + digit;
        has_digit = 1;
    }
    if (!has_digit) return 0;
    *out_value = value;
    return 1;
}

static uint32_t microdvd_gcd(uint32_t left, uint32_t right)
{
    while (right) {
        uint32_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

static int microdvd_parse_fps(const uint8_t *data, size_t size,
                              uint32_t *out_num, uint32_t *out_den)
{
    size_t position = 0u;
    size_t fractional_digits = 0u;
    uint64_t whole = 0u;
    uint64_t fraction = 0u;
    uint64_t scale = 1u;
    uint64_t numerator;
    uint32_t divisor;
    while (position < size && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    if (position == size || data[position] < '0' || data[position] > '9')
        return 0;
    while (position < size && data[position] >= '0' && data[position] <= '9') {
        uint8_t digit = data[position++] - '0';
        if (whole > (UINT64_MAX - digit) / 10u) return 0;
        whole = whole * 10u + digit;
    }
    if (position < size && data[position] == '.') {
        ++position;
        while (position < size && data[position] >= '0' &&
               data[position] <= '9') {
            uint8_t digit = data[position++] - '0';
            if (fractional_digits >= MICRODVD_FPS_DECIMAL_PLACES) return 0;
            fraction = fraction * 10u + digit;
            scale *= 10u;
            ++fractional_digits;
        }
        if (!fractional_digits) return 0;
    }
    while (position < size && (data[position] == ' ' || data[position] == '\t'))
        ++position;
    if (position != size || whole > 100u ||
        whole * scale > UINT64_MAX - fraction) return 0;
    numerator = whole * scale + fraction;
    if (!numerator || numerator > UINT32_MAX || scale > UINT32_MAX ||
        numerator <= 3u * scale || numerator >= 100u * scale) return 0;
    divisor = microdvd_gcd((uint32_t)numerator, (uint32_t)scale);
    *out_num = (uint32_t)numerator / divisor;
    *out_den = (uint32_t)scale / divisor;
    return *out_den <= MICRODVD_MAX_FPS_DENOMINATOR;
}

static int microdvd_parse_fps_header(const MicrodvdLine *line,
                                     uint32_t *out_num,
                                     uint32_t *out_den)
{
    const uint8_t *cursor = line->data;
    const uint8_t *end = line->data + line->size;
    uint64_t first_frame;
    uint64_t second_frame;
    const uint8_t *fps;
    if (cursor >= end || *cursor++ != '{' ||
        !microdvd_read_uint(&cursor, end, &first_frame) ||
        cursor >= end || *cursor++ != '}' || cursor >= end || *cursor++ != '{' ||
        first_frame > 1u) return 0;
    if (cursor < end && *cursor == '}') {
        second_frame = 0u;
    } else if (!microdvd_read_uint(&cursor, end, &second_frame)) {
        return 0;
    }
    (void)second_frame;
    if (cursor >= end || *cursor++ != '}') return 0;
    fps = cursor;
    return microdvd_parse_fps(fps, (size_t)(end - fps), out_num, out_den);
}

static int microdvd_mul_u64(uint64_t left, uint64_t right,
                            uint64_t *out_product)
{
    if (right && left > UINT64_MAX / right) return 0;
    *out_product = left * right;
    return 1;
}

static int microdvd_add_u64(uint64_t left, uint64_t right,
                            uint64_t *out_sum)
{
    if (left > UINT64_MAX - right) return 0;
    *out_sum = left + right;
    return 1;
}

static int microdvd_mul_div_round(uint64_t value, uint64_t multiplier,
                                  uint64_t divisor, uint64_t *out_value)
{
    uint64_t whole;
    uint64_t remainder;
    uint64_t base;
    uint64_t product;
    uint64_t tail;
    uint64_t fractional_remainder;
    if (!divisor || !out_value) return 0;
    whole = value / divisor;
    remainder = value % divisor;
    if (!microdvd_mul_u64(whole, multiplier, &base) ||
        !microdvd_mul_u64(remainder, multiplier, &product)) return 0;
    tail = product / divisor;
    fractional_remainder = product % divisor;
    if (fractional_remainder >= divisor / 2u + divisor % 2u) ++tail;
    return microdvd_add_u64(base, tail, out_value);
}

static int microdvd_parse_cue_line(const MicrodvdLine *line,
                                   uint64_t *out_start_frame,
                                   uint64_t *out_end_frame,
                                   const uint8_t **out_text,
                                   size_t *out_text_size)
{
    const uint8_t *cursor = line->data;
    const uint8_t *end = line->data + line->size;
    if (cursor >= end || *cursor++ != '{' ||
        !microdvd_read_uint(&cursor, end, out_start_frame) ||
        cursor >= end || *cursor++ != '}' || cursor >= end || *cursor++ != '{' ||
        !microdvd_read_uint(&cursor, end, out_end_frame) ||
        cursor >= end || *cursor++ != '}' || cursor >= end ||
        *out_start_frame >= *out_end_frame) return 0;
    *out_text = cursor;
    *out_text_size = (size_t)(end - cursor);
    return *out_text_size != 0u;
}

int qw_subtitle_parse_microdvd_document(const void *input, size_t input_size,
                                        QwSubtitleDocument *out_document,
                                        QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    size_t header_line_number = 0u;
    size_t nonempty_lines = 0u;
    uint32_t fps_num = 2997u;
    uint32_t fps_den = 125u;
    int fps_declared = 0;
    int skipped_style_metadata = 0;
    int rc = QW_STATUS_ERR_FORMAT;
    MicrodvdLine line;
    if (!input || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, QW_FORMAT_SUB);
    if (!input_size) goto failure;
    if (input_size > MICRODVD_MAX_INPUT_BYTES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto failure;
    }
    if (!microdvd_utf8_valid(data, size)) {
        rc = QW_STATUS_ERR_DECODE;
        goto failure;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    while (microdvd_next_line(data, size, &position, &line)) {
        if (microdvd_line_blank(&line)) continue;
        ++nonempty_lines;
        if (microdvd_default_style_line(&line)) continue;
        if (nonempty_lines > 3u) break;
        if (microdvd_parse_fps_header(&line, &fps_num, &fps_den)) {
            fps_declared = 1;
            header_line_number = nonempty_lines;
            break;
        }
    }
    out_document->frame_rate_num = fps_num;
    out_document->frame_rate_den = fps_den;
    position = 0u;
    nonempty_lines = 0u;
    while (microdvd_next_line(data, size, &position, &line)) {
        uint64_t start_frame;
        uint64_t end_frame;
        uint64_t start_ms;
        uint64_t end_ms;
        const uint8_t *text;
        size_t text_size;
        size_t i;
        uint8_t *decoded_text;
        if (microdvd_line_blank(&line)) continue;
        ++nonempty_lines;
        if (nonempty_lines == header_line_number && header_line_number) continue;
        if (microdvd_default_style_line(&line)) {
            skipped_style_metadata = 1;
            continue;
        }
        if (!microdvd_parse_cue_line(&line, &start_frame, &end_frame,
                                     &text, &text_size) ||
            !microdvd_mul_div_round(start_frame,
                (uint64_t)fps_den * 1000u, fps_num, &start_ms) ||
            !microdvd_mul_div_round(end_frame,
                (uint64_t)fps_den * 1000u, fps_num, &end_ms) ||
            start_ms >= end_ms) goto failure;
        if (text_size == SIZE_MAX) {
            rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto failure;
        }
        decoded_text = (uint8_t *)malloc(text_size + 1u);
        if (!decoded_text) {
            rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            goto failure;
        }
        for (i = 0u; i < text_size; ++i)
            decoded_text[i] = text[i] == '|' ? '\n' : text[i];
        decoded_text[text_size] = 0u;
        if (!qw_subtitle_document_add_cue(out_document, start_ms, end_ms, 1,
                NULL, 0u, NULL, 0u, decoded_text, text_size)) {
            free(decoded_text);
            if (out_document->cue_count >= QW_SUBTITLE_MAX_CUES ||
                text_size > QW_SUBTITLE_MAX_CONTENT_BYTES -
                    out_document->content_bytes) {
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
            } else {
                rc = QW_STATUS_ERR_OUT_OF_MEMORY;
            }
            goto failure;
        }
        free(decoded_text);
    }
    if (!qw_subtitle_document_validate(out_document)) goto failure;
    if (report) {
        if (fps_declared) {
            (void)snprintf(report->note, sizeof(report->note),
                "MicroDVD UTF-8 cues read with a declared frame rate%s",
                skipped_style_metadata
                    ? "; default style metadata is omitted" : "");
        } else {
            (void)snprintf(report->note, sizeof(report->note),
                "MicroDVD UTF-8 cues read using assumed 23.976 fps%s",
                skipped_style_metadata
                    ? "; default style metadata is omitted" : "");
        }
    }
    return QW_STATUS_OK;

failure:
    qw_subtitle_document_dispose(out_document);
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
                       "invalid or unsupported MicroDVD subtitle data");
    }
    return rc;
}

static int microdvd_open_subtitle(const void *data, size_t size,
                                  QwInputFormat input_format,
                                  const QwConvertOptions *options,
                                  QwSubtitleDocument *out_document,
                                  QwConvertReport *report)
{
    (void)options;
    if (input_format != QW_FORMAT_SUB) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return qw_subtitle_parse_microdvd_document(data, size, out_document,
                                               report);
}

static int microdvd_probe(const void *data, size_t size)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_microdvd_document(data, size, &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int microdvd_open_layout(const void *data, size_t size,
                                QwInputFormat input_format,
                                const QwConvertOptions *options,
                                QwLayoutDocument *out_document,
                                QwConvertReport *report)
{
    QwSubtitleDocument subtitle;
    int rc;
    (void)options;
    if (input_format != QW_FORMAT_SUB || !out_document)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    rc = qw_subtitle_parse_microdvd_document(data, size, &subtitle, report);
    if (rc != QW_STATUS_OK) return rc;
    rc = qw_subtitle_document_render_layout(&subtitle, out_document, report);
    qw_subtitle_document_dispose(&subtitle);
    return rc;
}

static int microdvd_append(QwTextBuffer *output, const char *data, size_t size,
                           int *out_error)
{
    if (output->size > MICRODVD_MAX_OUTPUT_BYTES ||
        size > MICRODVD_MAX_OUTPUT_BYTES - output->size) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(output, data, size)) {
        *out_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int microdvd_append_uint(QwTextBuffer *output, uint64_t value,
                                int *out_error)
{
    char text[32];
    int length = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (length < 0 || (size_t)length >= sizeof(text)) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return microdvd_append(output, text, (size_t)length, out_error);
}

static int microdvd_write_document(const QwSubtitleDocument *document,
                                   QwOutputFormat output_format,
                                   const QwConvertOptions *options,
                                   uint8_t **out_data, size_t *out_size,
                                   QwConvertReport *report)
{
    static const uint32_t default_fps_num = 2997u;
    static const uint32_t default_fps_den = 125u;
    QwTextBuffer output = {0};
    uint32_t fps_num;
    uint32_t fps_den;
    uint64_t fps_scaled;
    uint64_t frame_denominator;
    char fps_text[48];
    int fps_length;
    int error = QW_STATUS_OK;
    int metadata_dropped = 0;
    int short_cue_extended = 0;
    size_t i;
    (void)options;
    if (!document || output_format != QW_FORMAT_SUB || !out_data || !out_size ||
        !qw_subtitle_document_validate(document)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    fps_num = document->input_format == QW_FORMAT_SUB
        ? document->frame_rate_num : default_fps_num;
    fps_den = document->input_format == QW_FORMAT_SUB
        ? document->frame_rate_den : default_fps_den;
    frame_denominator = (uint64_t)fps_den * 1000u;
    if (!microdvd_mul_div_round(fps_num, 1000000u, fps_den, &fps_scaled))
        return QW_STATUS_ERR_FORMAT;
    fps_length = snprintf(fps_text, sizeof(fps_text), "%" PRIu64 ".%06" PRIu64,
                          fps_scaled / 1000000u, fps_scaled % 1000000u);
    if (fps_length < 0 || (size_t)fps_length >= sizeof(fps_text))
        return QW_STATUS_ERR_FORMAT;
    while (fps_length > 0 && fps_text[fps_length - 1] == '0')
        fps_text[--fps_length] = '\0';
    if (fps_length > 0 && fps_text[fps_length - 1] == '.')
        fps_text[--fps_length] = '\0';
    output.limit = (size_t)MICRODVD_MAX_OUTPUT_BYTES + 1u;
    if (!microdvd_append(&output, "{1}{}", 5u, &error) ||
        !microdvd_append(&output, fps_text, (size_t)fps_length, &error) ||
        !microdvd_append(&output, "\n", 1u, &error)) goto cleanup;
    for (i = 0u; i < document->cue_count; ++i) {
        const QwSubtitleCue *cue = &document->cues[i];
        uint64_t start_frame;
        uint64_t end_frame;
        size_t span_start = 0u;
        size_t j;
        if (!microdvd_mul_div_round(cue->start_milliseconds, fps_num,
                                    frame_denominator, &start_frame) ||
            !microdvd_mul_div_round(cue->end_milliseconds, fps_num,
                                    frame_denominator, &end_frame)) {
            error = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto cleanup;
        }
        if (end_frame <= start_frame) {
            if (start_frame == UINT64_MAX) {
                error = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto cleanup;
            }
            end_frame = start_frame + 1u;
            short_cue_extended = 1;
        }
        if (!microdvd_append(&output, "{", 1u, &error) ||
            !microdvd_append_uint(&output, start_frame, &error) ||
            !microdvd_append(&output, "}{", 2u, &error) ||
            !microdvd_append_uint(&output, end_frame, &error) ||
            !microdvd_append(&output, "}", 1u, &error)) goto cleanup;
        if (cue->identifier_size || cue->settings_size) metadata_dropped = 1;
        for (j = 0u; j < cue->text_size; ++j) {
            if (cue->text_utf8[j] == '|') {
                error = QW_STATUS_ERR_UNSUPPORTED;
                if (report) {
                    (void)snprintf(report->note, sizeof(report->note),
                        "MicroDVD cannot distinguish a literal vertical bar from a line break");
                }
                goto cleanup;
            }
            if (cue->text_utf8[j] == '\n') {
                if (j > span_start && !microdvd_append(&output,
                        (const char *)cue->text_utf8 + span_start,
                        j - span_start, &error)) goto cleanup;
                if (!microdvd_append(&output, "|", 1u, &error)) goto cleanup;
                span_start = j + 1u;
            }
        }
        if (cue->text_size > span_start &&
            !microdvd_append(&output,
                (const char *)cue->text_utf8 + span_start,
                cue->text_size - span_start, &error)) goto cleanup;
        if (!microdvd_append(&output, "\n", 1u, &error)) goto cleanup;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    output.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "MicroDVD output uses %.48s fps and frame-rounded cue times%s%s%s",
            fps_text,
            document->input_format == QW_FORMAT_SUB ? "" :
                "; assumed 23.976 fps for time-based input",
            metadata_dropped ? "; cue identifiers/settings are omitted" : "",
            short_cue_extended ? "; sub-frame cues were extended to one frame" : "");
    }
    error = QW_STATUS_OK;

cleanup:
    free(output.data);
    return error;
}

const QwFormatDescriptor qw_format_sub = {
    .id = QW_FORMAT_SUB,
    .name = "MicroDVD subtitles",
    .probe = microdvd_probe,
    .open_layout = microdvd_open_layout,
    .max_input_bytes = MICRODVD_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = microdvd_open_subtitle
};

const QwSubtitleWriterDescriptor qw_writer_sub_subtitle = {
    QW_FORMAT_SUB,
    "MicroDVD frame-cue writer",
    microdvd_write_document,
    MICRODVD_MAX_OUTPUT_BYTES
};
