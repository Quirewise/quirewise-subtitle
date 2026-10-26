#include "subtitle_format.h"

#include "../../core/text_buffer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SCC_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    SCC_MAX_TEXT_BYTES = 64u * 1024u * 1024u,
    SCC_MAX_OUTPUT_BYTES = 64u * 1024u * 1024u,
    SCC_FRAME_MILLISECONDS = 33u,
    SCC_INFERRED_END_MILLISECONDS = 5000u
};

typedef struct SccLine {
    const uint8_t *data;
    size_t size;
} SccLine;

typedef struct SccParser {
    QwSubtitleDocument *document;
    QwTextBuffer loading_text;
    QwTextBuffer active_text;
    uint64_t active_start;
    uint16_t previous_control;
    int has_previous_control;
    int loading;
    int active_visible;
    int saw_header;
    int saw_caption;
    int inferred_end;
    int error;
} SccParser;

static int scc_next_line(const uint8_t *data, size_t size, size_t *position,
                         SccLine *line)
{
    size_t start;
    if (!data || !position || !line || *position >= size) return 0;
    start = *position;
    while (*position < size && data[*position] != '\r' && data[*position] != '\n')
        ++*position;
    line->data = data + start;
    line->size = *position - start;
    if (*position < size && data[*position] == '\r') ++*position;
    if (*position < size && data[*position] == '\n') ++*position;
    return 1;
}

static int scc_line_blank(const SccLine *line)
{
    size_t i;
    if (!line) return 1;
    for (i = 0u; i < line->size; ++i) {
        if (line->data[i] != ' ' && line->data[i] != '\t') return 0;
    }
    return 1;
}

static int scc_equal_ascii(const uint8_t *data, size_t size, const char *text)
{
    size_t i;
    size_t text_size = strlen(text);
    if (!data || size != text_size) return 0;
    for (i = 0u; i < size; ++i) {
        uint8_t left = data[i];
        uint8_t right = (uint8_t)text[i];
        if (left >= 'A' && left <= 'Z') left = (uint8_t)(left + ('a' - 'A'));
        if (right >= 'A' && right <= 'Z') right = (uint8_t)(right + ('a' - 'A'));
        if (left != right) return 0;
    }
    return 1;
}

static int scc_read_digits(const uint8_t *data, size_t size, size_t *position,
                           size_t count, uint32_t *out_value)
{
    uint32_t value = 0u;
    size_t i;
    if (!data || !position || !out_value || count > size - *position) return 0;
    for (i = 0u; i < count; ++i) {
        uint8_t digit = data[(*position)++];
        if (digit < '0' || digit > '9') return 0;
        value = value * 10u + (uint32_t)(digit - '0');
    }
    *out_value = value;
    return 1;
}

static int scc_parse_timestamp(const SccLine *line, size_t *out_position,
                               uint64_t *out_milliseconds)
{
    size_t position = 0u;
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;
    uint32_t frames;
    uint64_t whole_seconds;
    if (!line || !out_position || !out_milliseconds ||
        !scc_read_digits(line->data, line->size, &position, 2u, &hours) ||
        position >= line->size || line->data[position++] != ':' ||
        !scc_read_digits(line->data, line->size, &position, 2u, &minutes) ||
        position >= line->size || line->data[position++] != ':' ||
        !scc_read_digits(line->data, line->size, &position, 2u, &seconds) ||
        position >= line->size ||
        (line->data[position] != ':' && line->data[position] != ';')) return 0;
    ++position;
    if (!scc_read_digits(line->data, line->size, &position, 2u, &frames) ||
        minutes >= 60u || seconds >= 60u || frames >= 30u ||
        hours > UINT32_MAX / 3600u) return 0;
    whole_seconds = (uint64_t)hours * 3600u + (uint64_t)minutes * 60u + seconds;
    if (whole_seconds > (UINT64_MAX - (uint64_t)frames * SCC_FRAME_MILLISECONDS) /
                            1000u) return 0;
    *out_milliseconds = whole_seconds * 1000u +
                        (uint64_t)frames * SCC_FRAME_MILLISECONDS;
    *out_position = position;
    return 1;
}

static int scc_hex_value(uint8_t value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int scc_parse_word(const uint8_t *data, size_t size, uint16_t *out_word)
{
    int high;
    int middle;
    int low;
    int last;
    if (!data || size != 4u || !out_word) return 0;
    high = scc_hex_value(data[0]);
    middle = scc_hex_value(data[1]);
    low = scc_hex_value(data[2]);
    last = scc_hex_value(data[3]);
    if (high < 0 || middle < 0 || low < 0 || last < 0) return 0;
    *out_word = (uint16_t)(((unsigned)high << 12) |
                           ((unsigned)middle << 8) |
                           ((unsigned)low << 4) | (unsigned)last);
    return 1;
}

static int scc_append(SccParser *parser, QwTextBuffer *buffer,
                      const void *data, size_t size)
{
    if (!parser || !buffer || (!data && size)) return 0;
    if (buffer->size > SCC_MAX_TEXT_BYTES ||
        size > SCC_MAX_TEXT_BYTES - buffer->size) {
        parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(buffer, (const char *)data, size)) {
        parser->error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static void scc_trim_text(QwTextBuffer *text)
{
    if (!text) return;
    while (text->size && (text->data[text->size - 1u] == ' ' ||
                          text->data[text->size - 1u] == '\t' ||
                          text->data[text->size - 1u] == '\n')) {
        --text->size;
    }
}

static int scc_text_blank(const QwTextBuffer *text)
{
    size_t i;
    if (!text) return 1;
    for (i = 0u; i < text->size; ++i) {
        if (text->data[i] != ' ' && text->data[i] != '\t' &&
            text->data[i] != '\n') return 0;
    }
    return 1;
}

static int scc_append_newline(SccParser *parser, QwTextBuffer *text)
{
    while (text->size && (text->data[text->size - 1u] == ' ' ||
                          text->data[text->size - 1u] == '\t')) --text->size;
    if (!text->size || text->data[text->size - 1u] == '\n') return 1;
    return scc_append(parser, text, "\n", 1u);
}

static int scc_emit_character(SccParser *parser, uint8_t value)
{
    static const char *const basic_overrides[128] = {
        [0x27] = "’", [0x2a] = "á", [0x5c] = "é", [0x5e] = "í",
        [0x5f] = "ó", [0x60] = "ú", [0x7b] = "ç", [0x7c] = "÷",
        [0x7d] = "Ñ", [0x7e] = "ñ", [0x7f] = "█"
    };
    if (!parser->loading || value < 0x20u) return 1;
    if (basic_overrides[value]) {
        const char *replacement = basic_overrides[value];
        return scc_append(parser, &parser->loading_text, replacement,
                          strlen(replacement));
    }
    if (value <= 0x7eu) {
        char character = (char)value;
        return scc_append(parser, &parser->loading_text, &character, 1u);
    }
    parser->error = QW_STATUS_ERR_UNSUPPORTED;
    return 0;
}

static int scc_emit_special_character(SccParser *parser, uint8_t value)
{
    static const char *const special[16] = {
        "®", "°", "½", "¿", "™", "¢", "£", "♪",
        "à", " ", "è", "â", "ê", "î", "ô", "û"
    };
    if (!parser->loading || value < 0x30u || value > 0x3fu) return 1;
    return scc_append(parser, &parser->loading_text,
                      special[value - 0x30u],
                      strlen(special[value - 0x30u]));
}

static int scc_finalize_active(SccParser *parser, uint64_t end_milliseconds,
                               int end_time_explicit)
{
    if (!parser->active_visible || scc_text_blank(&parser->active_text)) {
        parser->active_visible = 0;
        parser->active_text.size = 0u;
        return 1;
    }
    scc_trim_text(&parser->active_text);
    if (!parser->active_text.size || scc_text_blank(&parser->active_text)) {
        parser->active_visible = 0;
        parser->active_text.size = 0u;
        return 1;
    }
    if (end_milliseconds <= parser->active_start) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!qw_subtitle_document_add_cue(parser->document,
            parser->active_start, end_milliseconds, end_time_explicit,
            NULL, 0u, NULL, 0u,
            (const uint8_t *)parser->active_text.data,
            parser->active_text.size)) {
        parser->error = parser->document->cue_count >= QW_SUBTITLE_MAX_CUES
            ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    parser->active_visible = 0;
    parser->active_text.size = 0u;
    return 1;
}

static int scc_show_loading(SccParser *parser, uint64_t time_milliseconds)
{
    QwTextBuffer temporary;
    if (!scc_finalize_active(parser, time_milliseconds, 1)) return 0;
    scc_trim_text(&parser->loading_text);
    temporary = parser->active_text;
    parser->active_text = parser->loading_text;
    parser->loading_text = temporary;
    parser->loading_text.size = 0u;
    parser->active_start = time_milliseconds;
    parser->active_visible = !scc_text_blank(&parser->active_text);
    parser->saw_caption |= parser->active_visible;
    parser->loading = 0;
    return 1;
}

static int scc_process_word(SccParser *parser, uint16_t word,
                            uint64_t time_milliseconds)
{
    uint8_t first = (uint8_t)((word >> 8) & 0x7fu);
    uint8_t second = (uint8_t)(word & 0x7fu);
    int is_control = (first == 0x14u || first == 0x15u ||
                      first == 0x1cu || first == 0x1du) &&
                     second >= 0x20u && second <= 0x2fu;
    if (is_control) {
        if (parser->has_previous_control && parser->previous_control == word)
            return 1;
        parser->previous_control = word;
        parser->has_previous_control = 1;
    } else {
        parser->has_previous_control = 0;
    }

    if (first == 0x15u || first == 0x1cu || first == 0x1du) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }

    if (first == 0x14u && second == 0x20u) { /* Resume caption loading. */
        parser->loading_text.size = 0u;
        parser->loading = 1;
        return 1;
    }
    if (first == 0x14u && second == 0x2eu) { /* Erase non-displayed memory. */
        parser->loading_text.size = 0u;
        parser->loading = 1;
        return 1;
    }
    if (first == 0x14u && second == 0x2fu) /* End of caption. */
        return scc_show_loading(parser, time_milliseconds);
    if (first == 0x14u && second == 0x2cu) /* Erase displayed memory. */
        return scc_finalize_active(parser, time_milliseconds, 1);
    if (first == 0x14u && second == 0x2du) /* Carriage return. */
        return parser->loading
            ? scc_append_newline(parser, &parser->loading_text) : 1;
    if (first == 0x14u && second >= 0x21u && second <= 0x2bu) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }

    if ((first == 0x11u || first == 0x19u) &&
        second >= 0x30u && second <= 0x3fu) {
        return scc_emit_special_character(parser, second);
    }
    if ((first == 0x11u || first == 0x12u || first == 0x13u ||
         first == 0x19u || first == 0x1au || first == 0x1bu) &&
        second >= 0x20u && second <= 0x3fu) {
        parser->error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    if (first >= 0x10u && first <= 0x1fu) {
        /* PAC and mid-row attributes are intentionally flattened. */
        if (second >= 0x40u && second <= 0x7fu && parser->loading)
            return scc_append_newline(parser, &parser->loading_text);
        return 1;
    }
    if (first == 0u && second == 0u) return 1;
    if (first >= 0x20u && !scc_emit_character(parser, first)) return 0;
    return second < 0x20u || scc_emit_character(parser, second);
}

static int scc_parse_lines(SccParser *parser, const uint8_t *data, size_t size)
{
    size_t position = 0u;
    SccLine line;
    uint64_t last_timestamp = 0u;
    int has_timestamp = 0;
    while (scc_next_line(data, size, &position, &line)) {
        size_t cursor;
        uint64_t timestamp;
        if (scc_line_blank(&line)) continue;
        if (!parser->saw_header) {
            if (line.size >= 3u && line.data[0] == 0xefu &&
                line.data[1] == 0xbbu && line.data[2] == 0xbfu) {
                line.data += 3u;
                line.size -= 3u;
            }
            if (!scc_equal_ascii(line.data, line.size, "Scenarist_SCC V1.0")) {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            parser->saw_header = 1;
            continue;
        }
        if (!scc_parse_timestamp(&line, &cursor, &timestamp)) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        if (has_timestamp && timestamp < last_timestamp) {
            parser->error = QW_STATUS_ERR_FORMAT;
            return 0;
        }
        last_timestamp = timestamp;
        has_timestamp = 1;
        while (cursor < line.size) {
            size_t start;
            uint16_t word;
            while (cursor < line.size && (line.data[cursor] == ' ' ||
                                           line.data[cursor] == '\t')) ++cursor;
            if (cursor == line.size) break;
            start = cursor;
            while (cursor < line.size && line.data[cursor] != ' ' &&
                   line.data[cursor] != '\t') ++cursor;
            if (!scc_parse_word(line.data + start, cursor - start, &word)) {
                parser->error = QW_STATUS_ERR_FORMAT;
                return 0;
            }
            if (!scc_process_word(parser, word, timestamp)) return 0;
        }
    }
    if (!parser->saw_header || !has_timestamp) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (parser->active_visible) {
        uint64_t end_milliseconds;
        if (parser->active_start > UINT64_MAX - SCC_INFERRED_END_MILLISECONDS) {
            parser->error = QW_STATUS_ERR_RESOURCE_LIMIT;
            return 0;
        }
        end_milliseconds = parser->active_start + SCC_INFERRED_END_MILLISECONDS;
        if (!scc_finalize_active(parser, end_milliseconds, 0)) return 0;
        parser->inferred_end = 1;
    }
    return parser->saw_caption && parser->document->cue_count != 0u;
}

int qw_subtitle_parse_scc_document(const void *input, size_t input_size,
                                   QwSubtitleDocument *out_document,
                                   QwConvertReport *report)
{
    SccParser parser;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!input || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, QW_FORMAT_SCC);
    if (!input_size) goto failure;
    if (input_size > SCC_MAX_INPUT_BYTES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto failure;
    }
    memset(&parser, 0, sizeof(parser));
    parser.document = out_document;
    parser.loading_text.limit = (size_t)SCC_MAX_TEXT_BYTES + 1u;
    parser.active_text.limit = (size_t)SCC_MAX_TEXT_BYTES + 1u;
    if (!scc_parse_lines(&parser, (const uint8_t *)input, input_size) ||
        !qw_subtitle_document_validate(out_document)) {
        rc = parser.error ? parser.error : QW_STATUS_ERR_FORMAT;
        goto failure_parser;
    }
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "SCC decoded pop-on CC1 text with flattened row placement%s",
            parser.inferred_end ? "; the final cue end is inferred as five seconds" : "");
    }
    free(parser.loading_text.data);
    free(parser.active_text.data);
    return QW_STATUS_OK;

failure_parser:
    free(parser.loading_text.data);
    free(parser.active_text.data);
    qw_subtitle_document_dispose(out_document);
failure:
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
                       "invalid or unsupported Scenarist SCC subtitle data");
    }
    return rc;
}

static int scc_open_subtitle(const void *data, size_t size,
                             QwInputFormat input_format,
                             const QwConvertOptions *options,
                             QwSubtitleDocument *out_document,
                             QwConvertReport *report)
{
    (void)options;
    if (input_format != QW_FORMAT_SCC) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return qw_subtitle_parse_scc_document(data, size, out_document, report);
}

static int scc_probe(const void *data, size_t size)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_scc_document(data, size, &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int scc_open_layout(const void *data, size_t size,
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
    rc = qw_subtitle_parse_scc_document(data, size, &subtitle, report);
    if (rc != QW_STATUS_OK) return rc;
    if (subtitle.input_format != input_format) {
        qw_subtitle_document_dispose(&subtitle);
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    rc = qw_subtitle_document_render_layout(&subtitle, out_document, report);
    qw_subtitle_document_dispose(&subtitle);
    return rc;
}

static int scc_writer_append(QwTextBuffer *output, const void *data,
                             size_t size, int *out_error)
{
    if (!output || !out_error || (!data && size)) return 0;
    if (output->size > SCC_MAX_OUTPUT_BYTES ||
        size > SCC_MAX_OUTPUT_BYTES - output->size) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(output, (const char *)data, size)) {
        *out_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static uint8_t scc_writer_odd_parity(uint8_t value)
{
    uint8_t folded = (uint8_t)(value & 0x7fu);
    folded ^= (uint8_t)(folded >> 4);
    folded ^= (uint8_t)(folded >> 2);
    folded ^= (uint8_t)(folded >> 1);
    value &= 0x7fu;
    if ((folded & 1u) == 0u) value |= 0x80u;
    return value;
}

static int scc_writer_append_word(QwTextBuffer *output, uint8_t first,
                                  uint8_t second, int *first_word,
                                  int *out_error)
{
    char word[5];
    int length;
    if (!output || !first_word || !out_error) return 0;
    length = snprintf(word, sizeof(word), "%02X%02X",
                      (unsigned)scc_writer_odd_parity(first),
                      (unsigned)scc_writer_odd_parity(second));
    if (length != 4) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if ((!*first_word && !scc_writer_append(output, " ", 1u, out_error)) ||
        !scc_writer_append(output, word, 4u, out_error)) return 0;
    *first_word = 0;
    return 1;
}

static int scc_writer_timecode_parts(uint64_t milliseconds,
                                     uint64_t *out_seconds,
                                     uint64_t *out_frames,
                                     int *out_error)
{
    uint64_t seconds = milliseconds / 1000u;
    uint64_t remainder = milliseconds % 1000u;
    uint64_t frames = (remainder + SCC_FRAME_MILLISECONDS / 2u) /
                      SCC_FRAME_MILLISECONDS;
    if (!out_seconds || !out_frames || !out_error) return 0;
    if (frames >= 30u) {
        frames = 0u;
        ++seconds;
    }
    if (seconds / 3600u > 99u) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    *out_seconds = seconds;
    *out_frames = frames;
    return 1;
}

static int scc_writer_frame_ordinal(uint64_t milliseconds,
                                    uint64_t *out_ordinal,
                                    int *out_error)
{
    uint64_t seconds;
    uint64_t frames;
    if (!out_ordinal || !scc_writer_timecode_parts(milliseconds, &seconds,
                                                   &frames, out_error))
        return 0;
    *out_ordinal = seconds * 30u + frames;
    return 1;
}

static int scc_writer_append_time(QwTextBuffer *output, uint64_t milliseconds,
                                  int *out_error)
{
    char value[48];
    uint64_t seconds;
    uint64_t hours;
    uint64_t minutes;
    uint64_t whole_seconds;
    uint64_t frames;
    int length;
    if (!scc_writer_timecode_parts(milliseconds, &seconds, &frames,
                                   out_error)) return 0;
    hours = seconds / 3600u;
    minutes = (seconds / 60u) % 60u;
    whole_seconds = seconds % 60u;
    length = snprintf(value, sizeof(value), "%02llu:%02llu:%02llu:%02llu\t",
                      (unsigned long long)hours,
                      (unsigned long long)minutes,
                      (unsigned long long)whole_seconds,
                      (unsigned long long)frames);
    if (length < 0 || (size_t)length >= sizeof(value)) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return scc_writer_append(output, value, (size_t)length, out_error);
}

static int scc_writer_next_codepoint(const uint8_t *text, size_t size,
                                     size_t *position, uint32_t *out_codepoint)
{
    uint8_t first;
    if (!text || !position || !out_codepoint || *position >= size) return 0;
    first = text[(*position)++];
    if (first < 0x80u) {
        *out_codepoint = first;
    } else if (first >= 0xc2u && first <= 0xdfu && *position < size) {
        *out_codepoint = ((uint32_t)(first & 0x1fu) << 6) |
                         (uint32_t)(text[(*position)++] & 0x3fu);
    } else if (first >= 0xe0u && first <= 0xefu && size - *position >= 2u) {
        *out_codepoint = ((uint32_t)(first & 0x0fu) << 12) |
                         ((uint32_t)(text[(*position)] & 0x3fu) << 6) |
                         (uint32_t)(text[(*position) + 1u] & 0x3fu);
        *position += 2u;
    } else if (first >= 0xf0u && first <= 0xf4u && size - *position >= 3u) {
        *out_codepoint = ((uint32_t)(first & 0x07u) << 18) |
                         ((uint32_t)(text[*position] & 0x3fu) << 12) |
                         ((uint32_t)(text[*position + 1u] & 0x3fu) << 6) |
                         (uint32_t)(text[*position + 2u] & 0x3fu);
        *position += 3u;
    } else {
        return 0;
    }
    return 1;
}

static int scc_writer_basic_byte(uint32_t codepoint, uint8_t *out_value)
{
    static const struct {
        uint32_t codepoint;
        uint8_t value;
    } overrides[] = {
        { 0x2019u, 0x27u }, { 0x00e1u, 0x2au }, { 0x00e9u, 0x5cu },
        { 0x00edu, 0x5eu }, { 0x00f3u, 0x5fu }, { 0x00fau, 0x60u },
        { 0x00e7u, 0x7bu }, { 0x00f7u, 0x7cu }, { 0x00d1u, 0x7du },
        { 0x00f1u, 0x7eu }, { 0x2588u, 0x7fu }
    };
    size_t i;
    if (!out_value) return 0;
    if (codepoint >= 0x20u && codepoint <= 0x7eu &&
        codepoint != 0x27u && codepoint != 0x2au && codepoint != 0x5cu &&
        codepoint != 0x5eu && codepoint != 0x5fu && codepoint != 0x60u &&
        codepoint != 0x7bu && codepoint != 0x7cu && codepoint != 0x7du &&
        codepoint != 0x7eu) {
        *out_value = (uint8_t)codepoint;
        return 1;
    }
    for (i = 0u; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        if (overrides[i].codepoint == codepoint) {
            *out_value = overrides[i].value;
            return 1;
        }
    }
    return 0;
}

static int scc_writer_special_byte(uint32_t codepoint, uint8_t *out_value)
{
    static const uint32_t special[] = {
        0x00aeu, 0x00b0u, 0x00bdu, 0x00bfu, 0x2122u, 0x00a2u,
        0x00a3u, 0x266au, 0x00e0u, 0x00a0u, 0x00e8u, 0x00e2u,
        0x00eau, 0x00eeu, 0x00f4u, 0x00fbu
    };
    size_t i;
    if (!out_value) return 0;
    for (i = 0u; i < sizeof(special) / sizeof(special[0]); ++i) {
        if (special[i] == codepoint) {
            *out_value = (uint8_t)(0x30u + i);
            return 1;
        }
    }
    return 0;
}

typedef struct SccWriterText {
    QwTextBuffer *output;
    int *first_word;
    int *error;
    uint8_t pending;
    unsigned row_count;
    unsigned row_characters;
    int has_pending;
    int has_visible_text;
} SccWriterText;

static int scc_writer_flush_pending(SccWriterText *writer)
{
    if (!writer->has_pending) return 1;
    if (!scc_writer_append_word(writer->output, writer->pending, 0u,
                                writer->first_word, writer->error)) return 0;
    writer->has_pending = 0;
    return 1;
}

static int scc_writer_encode_text(QwTextBuffer *output, const uint8_t *text,
                                  size_t text_size, int *first_word,
                                  int *out_error)
{
    size_t position = 0u;
    SccWriterText writer;
    if (!output || !text || !first_word || !out_error) return 0;
    while (text_size && (text[text_size - 1u] == '\r' ||
                         text[text_size - 1u] == '\n')) --text_size;
    memset(&writer, 0, sizeof(writer));
    writer.output = output;
    writer.first_word = first_word;
    writer.error = out_error;
    writer.row_count = 1u;
    while (position < text_size) {
        uint32_t codepoint;
        uint8_t value;
        if (!scc_writer_next_codepoint(text, text_size, &position,
                                       &codepoint)) {
            *out_error = QW_STATUS_ERR_DECODE;
            return 0;
        }
        if (codepoint == '\r' || codepoint == '\n') {
            if (codepoint == '\r' && position < text_size &&
                text[position] == '\n') ++position;
            if (writer.row_count >= 4u || !scc_writer_flush_pending(&writer) ||
                !scc_writer_append_word(output, 0x14u, 0x2du,
                                        first_word, out_error) ||
                !scc_writer_append_word(output, 0x14u, 0x2du,
                                        first_word, out_error)) {
                if (*out_error == QW_STATUS_OK) *out_error = QW_STATUS_ERR_UNSUPPORTED;
                return 0;
            }
            ++writer.row_count;
            writer.row_characters = 0u;
            continue;
        }
        if (writer.row_characters >= 32u) {
            *out_error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        if (scc_writer_basic_byte(codepoint, &value)) {
            if (writer.has_pending) {
                if (!scc_writer_append_word(output, writer.pending, value,
                                            first_word, out_error)) return 0;
                writer.has_pending = 0;
            } else {
                writer.pending = value;
                writer.has_pending = 1;
            }
        } else if (scc_writer_special_byte(codepoint, &value)) {
            if (!scc_writer_flush_pending(&writer) ||
                !scc_writer_append_word(output, 0x11u, value,
                                        first_word, out_error)) return 0;
        } else {
            *out_error = QW_STATUS_ERR_UNSUPPORTED;
            return 0;
        }
        if (codepoint != ' ') writer.has_visible_text = 1;
        ++writer.row_characters;
    }
    if (!scc_writer_flush_pending(&writer)) return 0;
    if (!writer.has_visible_text) {
        *out_error = QW_STATUS_ERR_UNSUPPORTED;
        return 0;
    }
    return 1;
}

static int scc_write_document(const QwSubtitleDocument *document,
                              QwOutputFormat output_format,
                              const QwConvertOptions *options,
                              uint8_t **out_data, size_t *out_size,
                              QwConvertReport *report)
{
    static const char header[] = "Scenarist_SCC V1.0\n\n";
    QwTextBuffer output = {0};
    uint64_t previous_end_frame = 0u;
    int has_previous = 0;
    int inferred_end = 0;
    int error = QW_STATUS_OK;
    size_t i;
    (void)options;
    if (!document || output_format != QW_FORMAT_SCC || !out_data || !out_size ||
        !qw_subtitle_document_validate(document)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (!document->cue_count) return QW_STATUS_ERR_UNSUPPORTED;
    output.limit = (size_t)SCC_MAX_OUTPUT_BYTES + 1u;
    if (!scc_writer_append(&output, header, sizeof(header) - 1u, &error))
        goto cleanup;
    for (i = 0u; i < document->cue_count; ++i) {
        const QwSubtitleCue *cue = &document->cues[i];
        uint64_t start_frame;
        uint64_t end_frame;
        size_t identifier_index;
        int first_word = 1;
        /* SRT sequence numbers do not carry caption semantics. */
        for (identifier_index = 0u; identifier_index < cue->identifier_size;
             ++identifier_index) {
            if (cue->identifier_utf8[identifier_index] < '0' ||
                cue->identifier_utf8[identifier_index] > '9') break;
        }
        if (identifier_index != cue->identifier_size || cue->settings_size) {
            error = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        if (!scc_writer_frame_ordinal(cue->start_milliseconds, &start_frame,
                                      &error) ||
            !scc_writer_frame_ordinal(cue->end_milliseconds, &end_frame,
                                      &error)) goto cleanup;
        if (start_frame >= end_frame ||
            (has_previous && start_frame < previous_end_frame)) {
            error = QW_STATUS_ERR_UNSUPPORTED;
            goto cleanup;
        }
        if (!cue->end_time_explicit) inferred_end = 1;
        if (!scc_writer_append_time(&output, cue->start_milliseconds, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x20u,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x20u,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2eu,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2eu,
                                    &first_word, &error) ||
            !scc_writer_encode_text(&output, cue->text_utf8, cue->text_size,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2fu,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2fu,
                                    &first_word, &error) ||
            !scc_writer_append(&output, "\n", 1u, &error)) goto cleanup;
        first_word = 1;
        if (!scc_writer_append_time(&output, cue->end_milliseconds, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2cu,
                                    &first_word, &error) ||
            !scc_writer_append_word(&output, 0x14u, 0x2cu,
                                    &first_word, &error) ||
            !scc_writer_append(&output, "\n", 1u, &error)) goto cleanup;
        previous_end_frame = end_frame;
        has_previous = 1;
    }
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    output.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "SCC CC1 pop-on, 4x32 cells, 33 ms frames; numeric labels omitted; rejects unsupported text, overlap, settings and non-numeric IDs%s",
            inferred_end ? "; inferred ends use explicit erase times" : "");
    }
    error = QW_STATUS_OK;

cleanup:
    free(output.data);
    if (error != QW_STATUS_OK && report) {
        (void)snprintf(report->note, sizeof(report->note),
            error == QW_STATUS_ERR_RESOURCE_LIMIT
                ? "SCC output exceeds its 64 MiB or timecode limit"
                : (error == QW_STATUS_ERR_OUT_OF_MEMORY
                    ? "could not allocate SCC output"
                    : "SCC output requires non-overlapping CC1 cues with supported characters and no settings or non-numeric identifiers"));
    }
    return error;
}

const QwFormatDescriptor qw_format_scc = {
    .id = QW_FORMAT_SCC,
    .name = "Scenarist SCC subtitles",
    .probe = scc_probe,
    .open_layout = scc_open_layout,
    .max_input_bytes = SCC_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = scc_open_subtitle
};

const QwSubtitleWriterDescriptor qw_writer_scc_subtitle = {
    QW_FORMAT_SCC,
    "SCC CC1 timed-cue writer",
    scc_write_document,
    SCC_MAX_OUTPUT_BYTES
};
