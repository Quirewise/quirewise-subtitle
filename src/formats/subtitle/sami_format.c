#include "subtitle_format.h"

#include "../../core/text_buffer.h"
#include "../text/html_entities.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SAMI_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    SAMI_MAX_OUTPUT_BYTES = 64u * 1024u * 1024u,
    SAMI_MAX_WRITER_CUES = 200000u,
    SAMI_INFERRED_END_MILLISECONDS = 5000u
};

typedef struct SamiSlice {
    const uint8_t *data;
    size_t size;
} SamiSlice;

typedef struct SamiTag {
    const uint8_t *name;
    size_t name_size;
    const uint8_t *attributes;
    size_t attributes_size;
    int closing;
    int self_closing;
} SamiTag;

typedef struct SamiParser {
    QwSubtitleDocument *document;
    QwTextBuffer paragraph;
    QwTextBuffer sync_text;
    const uint8_t *selected_class;
    size_t selected_class_size;
    uint64_t sync_start;
    int selection_set;
    int selected_class_present;
    int paragraph_open;
    int paragraph_selected;
    int in_body;
    int has_sync;
    int saw_sami;
    int saw_body;
    int inferred_end;
    int error;
} SamiParser;

typedef struct SamiTransition {
    uint64_t time;
    size_t cue_index;
    int starts;
} SamiTransition;

static uint8_t sami_ascii_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int sami_equal_ascii(const uint8_t *data, size_t size,
                            const char *text)
{
    size_t i;
    size_t text_size = strlen(text);
    if (!data || size != text_size) return 0;
    for (i = 0u; i < size; ++i) {
        if (sami_ascii_lower(data[i]) != sami_ascii_lower((uint8_t)text[i]))
            return 0;
    }
    return 1;
}

static int sami_equal_slices(const uint8_t *left, size_t left_size,
                             const uint8_t *right, size_t right_size)
{
    size_t i;
    if (left_size != right_size) return 0;
    for (i = 0u; i < left_size; ++i) {
        if (sami_ascii_lower(left[i]) != sami_ascii_lower(right[i])) return 0;
    }
    return 1;
}

static int sami_utf8_valid(const uint8_t *data, size_t size)
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
                           first != '\t' && first != '\f')) return 0;
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
             codepoint != '\t' && codepoint != '\f')) return 0;
    }
    return 1;
}

static void sami_trim(SamiSlice *slice)
{
    while (slice->size && (slice->data[0] == ' ' || slice->data[0] == '\t' ||
           slice->data[0] == '\r' || slice->data[0] == '\n' ||
           slice->data[0] == '\f')) {
        ++slice->data;
        --slice->size;
    }
    while (slice->size && (slice->data[slice->size - 1u] == ' ' ||
           slice->data[slice->size - 1u] == '\t' ||
           slice->data[slice->size - 1u] == '\r' ||
           slice->data[slice->size - 1u] == '\n' ||
           slice->data[slice->size - 1u] == '\f')) --slice->size;
}

static int sami_parse_tag(const uint8_t *data, size_t size, size_t offset,
                          SamiTag *tag, size_t *out_consumed)
{
    size_t cursor;
    size_t end;
    size_t last;
    size_t name_start;
    char quote = '\0';
    if (!data || !tag || !out_consumed || offset >= size || data[offset] != '<')
        return 0;
    end = offset + 1u;
    for (; end < size; ++end) {
        uint8_t value = data[end];
        if (quote) {
            if (value == (uint8_t)quote) quote = '\0';
        } else if (value == '\'' || value == '"') {
            quote = (char)value;
        } else if (value == '>') {
            break;
        }
    }
    if (end >= size) return 0;
    cursor = offset + 1u;
    while (cursor < end && (data[cursor] == ' ' || data[cursor] == '\t' ||
                            data[cursor] == '\r' || data[cursor] == '\n'))
        ++cursor;
    memset(tag, 0, sizeof(*tag));
    if (cursor < end && data[cursor] == '/') {
        tag->closing = 1;
        ++cursor;
    }
    name_start = cursor;
    while (cursor < end &&
           ((data[cursor] >= 'A' && data[cursor] <= 'Z') ||
            (data[cursor] >= 'a' && data[cursor] <= 'z') ||
            (data[cursor] >= '0' && data[cursor] <= '9') ||
            data[cursor] == ':' || data[cursor] == '-')) ++cursor;
    if (cursor == name_start) return 0;
    tag->name = data + name_start;
    tag->name_size = cursor - name_start;
    tag->attributes = data + cursor;
    tag->attributes_size = end - cursor;
    last = end;
    while (last > cursor && (data[last - 1u] == ' ' || data[last - 1u] == '\t' ||
                             data[last - 1u] == '\r' || data[last - 1u] == '\n'))
        --last;
    tag->self_closing = last > cursor && data[last - 1u] == '/';
    *out_consumed = end - offset + 1u;
    return 1;
}

static int sami_attribute(const SamiTag *tag, const char *name,
                          SamiSlice *out_value)
{
    size_t cursor = 0u;
    if (!tag || !name || !out_value) return 0;
    while (cursor < tag->attributes_size) {
        size_t key_start;
        size_t key_end;
        size_t value_start;
        size_t value_end;
        uint8_t quote = 0u;
        while (cursor < tag->attributes_size &&
               (tag->attributes[cursor] == ' ' || tag->attributes[cursor] == '\t' ||
                tag->attributes[cursor] == '\r' || tag->attributes[cursor] == '\n' ||
                tag->attributes[cursor] == '/' || tag->attributes[cursor] == '\\'))
            ++cursor;
        if (cursor >= tag->attributes_size) break;
        key_start = cursor;
        while (cursor < tag->attributes_size &&
               tag->attributes[cursor] != '=' && tag->attributes[cursor] != ' ' &&
               tag->attributes[cursor] != '\t' && tag->attributes[cursor] != '\r' &&
               tag->attributes[cursor] != '\n' && tag->attributes[cursor] != '/')
            ++cursor;
        key_end = cursor;
        while (cursor < tag->attributes_size &&
               (tag->attributes[cursor] == ' ' || tag->attributes[cursor] == '\t' ||
                tag->attributes[cursor] == '\r' || tag->attributes[cursor] == '\n'))
            ++cursor;
        if (cursor >= tag->attributes_size || tag->attributes[cursor] != '=') {
            while (cursor < tag->attributes_size && tag->attributes[cursor] != ' ' &&
                   tag->attributes[cursor] != '\t' && tag->attributes[cursor] != '\r' &&
                   tag->attributes[cursor] != '\n') ++cursor;
            continue;
        }
        ++cursor;
        while (cursor < tag->attributes_size &&
               (tag->attributes[cursor] == ' ' || tag->attributes[cursor] == '\t' ||
                tag->attributes[cursor] == '\r' || tag->attributes[cursor] == '\n'))
            ++cursor;
        if (cursor >= tag->attributes_size) break;
        if (tag->attributes[cursor] == '\'' || tag->attributes[cursor] == '"')
            quote = tag->attributes[cursor++];
        value_start = cursor;
        if (quote) {
            while (cursor < tag->attributes_size && tag->attributes[cursor] != quote)
                ++cursor;
            value_end = cursor;
            if (cursor < tag->attributes_size) ++cursor;
        } else {
            while (cursor < tag->attributes_size &&
                   tag->attributes[cursor] != ' ' && tag->attributes[cursor] != '\t' &&
                   tag->attributes[cursor] != '\r' && tag->attributes[cursor] != '\n' &&
                   tag->attributes[cursor] != '/') ++cursor;
            value_end = cursor;
        }
        if (sami_equal_ascii(tag->attributes + key_start, key_end - key_start, name)) {
            out_value->data = tag->attributes + value_start;
            out_value->size = value_end - value_start;
            return 1;
        }
    }
    return 0;
}

static int sami_append(QwTextBuffer *buffer, const void *data, size_t size,
                       int *out_error)
{
    if (!buffer || !out_error || (!data && size)) return 0;
    if (buffer->size > SAMI_MAX_INPUT_BYTES ||
        size > SAMI_MAX_INPUT_BYTES - buffer->size) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(buffer, (const char *)data, size)) {
        *out_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int sami_append_codepoint(QwTextBuffer *buffer, uint32_t codepoint,
                                 int *out_error)
{
    uint8_t encoded[4];
    size_t size;
    if ((codepoint < 0x20u && codepoint != ' ' && codepoint != '\t' &&
         codepoint != '\r' && codepoint != '\n') || codepoint == 0x7fu)
        codepoint = 0xfffdu;
    if (codepoint <= 0x7fu) {
        encoded[0] = (uint8_t)codepoint;
        size = 1u;
    } else if (codepoint <= 0x7ffu) {
        encoded[0] = (uint8_t)(0xc0u | (codepoint >> 6));
        encoded[1] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        size = 2u;
    } else if (codepoint <= 0xffffu &&
               !(codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
        encoded[0] = (uint8_t)(0xe0u | (codepoint >> 12));
        encoded[1] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        size = 3u;
    } else if (codepoint <= 0x10ffffu) {
        encoded[0] = (uint8_t)(0xf0u | (codepoint >> 18));
        encoded[1] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu));
        encoded[3] = (uint8_t)(0x80u | (codepoint & 0x3fu));
        size = 4u;
    } else {
        static const uint8_t replacement[] = {0xefu, 0xbfu, 0xbdu};
        return sami_append(buffer, replacement, sizeof(replacement), out_error);
    }
    return sami_append(buffer, encoded, size, out_error);
}

static int sami_append_space(QwTextBuffer *buffer, int *out_error)
{
    if (buffer->size && (buffer->data[buffer->size - 1u] == ' ' ||
                         buffer->data[buffer->size - 1u] == '\n')) return 1;
    return sami_append(buffer, " ", 1u, out_error);
}

static int sami_append_paragraph_text(QwTextBuffer *buffer,
                                      const uint8_t *data, size_t size,
                                      int *out_error)
{
    size_t i = 0u;
    while (i < size) {
        if (data[i] == '&') {
            uint32_t codepoint;
            size_t consumed;
            if (qw_html_decode_entity(data, size, i, &codepoint, &consumed)) {
                if (codepoint == ' ' || codepoint == '\t' || codepoint == '\r' ||
                    codepoint == '\n' || codepoint == '\f') {
                    if (!sami_append_space(buffer, out_error)) return 0;
                } else if (!sami_append_codepoint(buffer, codepoint, out_error)) {
                    return 0;
                }
                i += consumed;
                continue;
            }
        }
        if (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' ||
            data[i] == '\n' || data[i] == '\f') {
            if (!sami_append_space(buffer, out_error)) return 0;
            ++i;
            continue;
        }
        if (!sami_append(buffer, data + i, 1u, out_error)) return 0;
        ++i;
    }
    return 1;
}

static int sami_append_line_break(QwTextBuffer *buffer, int *out_error)
{
    while (buffer->size && buffer->data[buffer->size - 1u] == ' ')
        --buffer->size;
    if (buffer->size && buffer->data[buffer->size - 1u] == '\n') return 1;
    return sami_append(buffer, "\n", 1u, out_error);
}

static int sami_text_blank(const uint8_t *data, size_t size)
{
    size_t i = 0u;
    while (i < size) {
        if (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' ||
            data[i] == '\n' || data[i] == '\f') {
            ++i;
        } else if (i + 1u < size && data[i] == 0xc2u && data[i + 1u] == 0xa0u) {
            i += 2u;
        } else {
            return 0;
        }
    }
    return 1;
}

static int sami_finish_paragraph(SamiParser *parser)
{
    SamiSlice value;
    int error = QW_STATUS_OK;
    if (!parser->paragraph_open) return 1;
    parser->paragraph_open = 0;
    if (!parser->paragraph_selected) {
        parser->paragraph.size = 0u;
        return 1;
    }
    value.data = (const uint8_t *)parser->paragraph.data;
    value.size = parser->paragraph.size;
    sami_trim(&value);
    if (!value.size || sami_text_blank(value.data, value.size)) {
        parser->paragraph.size = 0u;
        return 1;
    }
    if (parser->sync_text.size &&
        !sami_append_line_break(&parser->sync_text, &error)) goto failure;
    if (!sami_append(&parser->sync_text, value.data, value.size, &error))
        goto failure;
    parser->paragraph.size = 0u;
    return 1;

failure:
    parser->error = error ? error : QW_STATUS_ERR_OUT_OF_MEMORY;
    return 0;
}

static int sami_finalize_sync(SamiParser *parser, uint64_t end_milliseconds,
                              int end_time_explicit)
{
    if (!parser->has_sync || !parser->sync_text.size ||
        sami_text_blank((const uint8_t *)parser->sync_text.data,
                        parser->sync_text.size)) {
        parser->sync_text.size = 0u;
        return 1;
    }
    if (end_milliseconds <= parser->sync_start) {
        parser->error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    if (!qw_subtitle_document_add_cue(parser->document, parser->sync_start,
            end_milliseconds, end_time_explicit, NULL, 0u, NULL, 0u,
            (const uint8_t *)parser->sync_text.data,
            parser->sync_text.size)) {
        parser->error = parser->document->cue_count >= QW_SUBTITLE_MAX_CUES
            ? QW_STATUS_ERR_RESOURCE_LIMIT : QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    parser->sync_text.size = 0u;
    return 1;
}

static int sami_read_start(const SamiTag *tag, uint64_t *out_start)
{
    SamiSlice value;
    const uint8_t *cursor;
    const uint8_t *end;
    uint64_t parsed = 0u;
    if (!sami_attribute(tag, "Start", &value)) return 0;
    sami_trim(&value);
    cursor = value.data;
    end = value.data + value.size;
    if (cursor == end) return 0;
    while (cursor < end) {
        unsigned digit;
        if (*cursor < '0' || *cursor > '9') return 0;
        digit = (unsigned)(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10u) return 0;
        parsed = parsed * 10u + digit;
        ++cursor;
    }
    *out_start = parsed;
    return 1;
}

int qw_subtitle_parse_sami_document(const void *input, size_t input_size,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position = 0u;
    SamiParser parser;
    int rc = QW_STATUS_ERR_FORMAT;
    if (!input || !out_document) return QW_STATUS_ERR_INVALID_ARGUMENT;
    qw_subtitle_document_init(out_document, QW_FORMAT_SAMI);
    if (!input_size) goto failure;
    if (input_size > SAMI_MAX_INPUT_BYTES) {
        rc = QW_STATUS_ERR_RESOURCE_LIMIT;
        goto failure;
    }
    if (!sami_utf8_valid(data, size)) {
        rc = QW_STATUS_ERR_DECODE;
        goto failure;
    }
    if (size >= 3u && data[0] == 0xefu && data[1] == 0xbbu &&
        data[2] == 0xbfu) {
        data += 3u;
        size -= 3u;
    }
    memset(&parser, 0, sizeof(parser));
    parser.document = out_document;
    parser.paragraph.limit = (size_t)SAMI_MAX_INPUT_BYTES + 1u;
    parser.sync_text.limit = (size_t)SAMI_MAX_INPUT_BYTES + 1u;
    while (position < size) {
        if (data[position] != '<') {
            size_t text_end = position + 1u;
            int error = QW_STATUS_OK;
            while (text_end < size && data[text_end] != '<') ++text_end;
            if (parser.in_body && parser.paragraph_open &&
                parser.paragraph_selected &&
                !sami_append_paragraph_text(&parser.paragraph, data + position,
                                            text_end - position, &error)) {
                parser.error = error ? error : QW_STATUS_ERR_OUT_OF_MEMORY;
                goto failure_parser;
            }
            position = text_end;
            continue;
        }
        if (position + 4u <= size && memcmp(data + position, "<!--", 4u) == 0) {
            size_t end = position + 4u;
            while (end + 2u < size && memcmp(data + end, "-->", 3u) != 0) ++end;
            if (end + 2u >= size) {
                rc = QW_STATUS_ERR_FORMAT;
                goto failure_parser;
            }
            position = end + 3u;
            continue;
        }
        if (position + 1u < size && data[position + 1u] == '?') {
            size_t end = position + 2u;
            while (end + 1u < size &&
                   !(data[end] == '?' && data[end + 1u] == '>')) ++end;
            if (end + 1u >= size) {
                rc = QW_STATUS_ERR_FORMAT;
                goto failure_parser;
            }
            position = end + 2u;
            continue;
        }
        if (position + 1u < size && data[position + 1u] == '!') {
            size_t end = position + 2u;
            char quote = '\0';
            for (; end < size; ++end) {
                uint8_t value = data[end];
                if (quote) {
                    if (value == (uint8_t)quote) quote = '\0';
                } else if (value == '\'' || value == '"') {
                    quote = (char)value;
                } else if (value == '>') {
                    break;
                }
            }
            if (end >= size) {
                rc = QW_STATUS_ERR_FORMAT;
                goto failure_parser;
            }
            position = end + 1u;
            continue;
        }
        {
            SamiTag tag;
            size_t consumed;
            if (!sami_parse_tag(data, size, position, &tag, &consumed)) {
                rc = QW_STATUS_ERR_FORMAT;
                goto failure_parser;
            }
            position += consumed;
            if (sami_equal_ascii(tag.name, tag.name_size, "SAMI") &&
                !tag.closing) parser.saw_sami = 1;
            if (sami_equal_ascii(tag.name, tag.name_size, "BODY")) {
                if (tag.closing) {
                    if (!sami_finish_paragraph(&parser)) goto failure_parser;
                    if (parser.has_sync) {
                        uint64_t inferred_end;
                        size_t cue_count = parser.document->cue_count;
                        if (parser.sync_text.size &&
                            parser.sync_start > UINT64_MAX -
                                SAMI_INFERRED_END_MILLISECONDS) {
                            parser.error = QW_STATUS_ERR_RESOURCE_LIMIT;
                            goto failure_parser;
                        }
                        inferred_end = parser.sync_start +
                                       SAMI_INFERRED_END_MILLISECONDS;
                        if (!sami_finalize_sync(&parser, inferred_end, 0))
                            goto failure_parser;
                        if (parser.document->cue_count > cue_count)
                            parser.inferred_end = 1;
                    }
                    parser.in_body = 0;
                } else {
                    parser.in_body = 1;
                    parser.saw_body = 1;
                }
                continue;
            }
            if (!parser.in_body) continue;
            if (sami_equal_ascii(tag.name, tag.name_size, "SYNC") &&
                !tag.closing) {
                uint64_t start;
                if (!sami_read_start(&tag, &start) ||
                    !sami_finish_paragraph(&parser)) {
                    parser.error = parser.error ? parser.error : QW_STATUS_ERR_FORMAT;
                    goto failure_parser;
                }
                if (parser.has_sync) {
                    if (start < parser.sync_start) {
                        parser.error = QW_STATUS_ERR_FORMAT;
                        goto failure_parser;
                    }
                    if (start > parser.sync_start &&
                        !sami_finalize_sync(&parser, start, 1))
                        goto failure_parser;
                }
                if (!parser.has_sync || start > parser.sync_start)
                    parser.sync_start = start;
                parser.has_sync = 1;
                continue;
            }
            if (sami_equal_ascii(tag.name, tag.name_size, "P")) {
                if (tag.closing) {
                    if (!sami_finish_paragraph(&parser)) goto failure_parser;
                } else {
                    SamiSlice class_name;
                    int has_class;
                    if (!parser.has_sync || !sami_finish_paragraph(&parser)) {
                        parser.error = parser.error ? parser.error : QW_STATUS_ERR_FORMAT;
                        goto failure_parser;
                    }
                    has_class = sami_attribute(&tag, "Class", &class_name);
                    if (has_class) sami_trim(&class_name);
                    if (!parser.selection_set) {
                        parser.selection_set = 1;
                        parser.selected_class_present = has_class;
                        if (has_class) {
                            parser.selected_class = class_name.data;
                            parser.selected_class_size = class_name.size;
                        }
                    }
                    parser.paragraph_selected =
                        has_class == parser.selected_class_present &&
                        (!has_class || sami_equal_slices(class_name.data,
                            class_name.size, parser.selected_class,
                            parser.selected_class_size));
                    parser.paragraph_open = 1;
                    parser.paragraph.size = 0u;
                    if (tag.self_closing && !sami_finish_paragraph(&parser))
                        goto failure_parser;
                }
                continue;
            }
            if (!tag.closing && sami_equal_ascii(tag.name, tag.name_size, "BR") &&
                parser.paragraph_open && parser.paragraph_selected) {
                int error = QW_STATUS_OK;
                if (!sami_append_line_break(&parser.paragraph, &error)) {
                    parser.error = error ? error : QW_STATUS_ERR_OUT_OF_MEMORY;
                    goto failure_parser;
                }
            }
        }
    }
    if (!sami_finish_paragraph(&parser)) goto failure_parser;
    if (parser.has_sync && parser.sync_text.size) {
        uint64_t inferred_end;
        size_t cue_count = parser.document->cue_count;
        if (parser.sync_start > UINT64_MAX - SAMI_INFERRED_END_MILLISECONDS) {
            parser.error = QW_STATUS_ERR_RESOURCE_LIMIT;
            goto failure_parser;
        }
        inferred_end = parser.sync_start + SAMI_INFERRED_END_MILLISECONDS;
        if (!sami_finalize_sync(&parser, inferred_end, 0)) goto failure_parser;
        if (parser.document->cue_count > cue_count) parser.inferred_end = 1;
    }
    if (!parser.saw_sami || !parser.saw_body || !out_document->cue_count ||
        !qw_subtitle_document_validate(out_document)) goto failure_parser;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "SAMI selected the first caption class and read SYNC timing; HTML markup and styles are flattened%s%s",
            parser.inferred_end ? "; the final cue end is inferred as five seconds" : "",
            parser.selected_class_present ? "; other caption classes are omitted" : "");
    }
    free(parser.paragraph.data);
    free(parser.sync_text.data);
    return QW_STATUS_OK;

failure_parser:
    free(parser.paragraph.data);
    free(parser.sync_text.data);
    qw_subtitle_document_dispose(out_document);
    rc = parser.error ? parser.error : rc;
failure:
    if (report && !report->note[0]) {
        (void)snprintf(report->note, sizeof(report->note),
                       "invalid or unsupported SAMI subtitle data");
    }
    return rc;
}

static int sami_open_subtitle(const void *data, size_t size,
                              QwInputFormat input_format,
                              const QwConvertOptions *options,
                              QwSubtitleDocument *out_document,
                              QwConvertReport *report)
{
    (void)options;
    if (input_format != QW_FORMAT_SAMI) return QW_STATUS_ERR_INVALID_ARGUMENT;
    return qw_subtitle_parse_sami_document(data, size, out_document, report);
}

static int sami_probe(const void *data, size_t size)
{
    QwSubtitleDocument document;
    int rc = qw_subtitle_parse_sami_document(data, size, &document, NULL);
    if (rc != QW_STATUS_OK) return 0;
    qw_subtitle_document_dispose(&document);
    return 1;
}

static int sami_open_layout(const void *data, size_t size,
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
    rc = qw_subtitle_parse_sami_document(data, size, &subtitle, report);
    if (rc != QW_STATUS_OK) return rc;
    if (subtitle.input_format != input_format) {
        qw_subtitle_document_dispose(&subtitle);
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    }
    rc = qw_subtitle_document_render_layout(&subtitle, out_document, report);
    qw_subtitle_document_dispose(&subtitle);
    return rc;
}

static int sami_output_append(QwTextBuffer *output, const void *data,
                              size_t size, int *out_error)
{
    if (output->size > SAMI_MAX_OUTPUT_BYTES ||
        size > SAMI_MAX_OUTPUT_BYTES - output->size) {
        *out_error = QW_STATUS_ERR_RESOURCE_LIMIT;
        return 0;
    }
    if (!qw_text_buffer_append(output, (const char *)data, size)) {
        *out_error = QW_STATUS_ERR_OUT_OF_MEMORY;
        return 0;
    }
    return 1;
}

static int sami_append_time(QwTextBuffer *output, uint64_t time,
                            int *out_error)
{
    char value[32];
    int length = snprintf(value, sizeof(value), "%" PRIu64, time);
    if (length < 0 || (size_t)length >= sizeof(value)) {
        *out_error = QW_STATUS_ERR_FORMAT;
        return 0;
    }
    return sami_output_append(output, value, (size_t)length, out_error);
}

static int sami_escape_text(QwTextBuffer *output, const uint8_t *data,
                            size_t size, int *out_error)
{
    size_t i = 0u;
    while (i < size) {
        const char *replacement = NULL;
        size_t replacement_size = 0u;
        switch (data[i]) {
        case '&': replacement = "&amp;"; replacement_size = 5u; break;
        case '<': replacement = "&lt;"; replacement_size = 4u; break;
        case '>': replacement = "&gt;"; replacement_size = 4u; break;
        case '"': replacement = "&quot;"; replacement_size = 6u; break;
        case '\'': replacement = "&#39;"; replacement_size = 5u; break;
        case '\n':
            if (!sami_output_append(output, "<br>\n", 5u, out_error)) return 0;
            ++i;
            continue;
        case '\r':
            if (i + 1u < size && data[i + 1u] == '\n') ++i;
            if (!sami_output_append(output, "<br>\n", 5u, out_error)) return 0;
            ++i;
            continue;
        default: break;
        }
        if (replacement) {
            if (!sami_output_append(output, replacement, replacement_size,
                                    out_error)) return 0;
        } else if (!sami_output_append(output, data + i, 1u, out_error)) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int sami_transition_compare(const void *left, const void *right)
{
    const SamiTransition *a = (const SamiTransition *)left;
    const SamiTransition *b = (const SamiTransition *)right;
    if (a->time < b->time) return -1;
    if (a->time > b->time) return 1;
    if (a->starts != b->starts) return a->starts - b->starts;
    if (a->cue_index < b->cue_index) return -1;
    if (a->cue_index > b->cue_index) return 1;
    return 0;
}

static int sami_active_add(size_t *active, size_t *active_count,
                           size_t cue_index)
{
    size_t position = 0u;
    while (position < *active_count && active[position] < cue_index) ++position;
    if (position < *active_count && active[position] == cue_index) return 0;
    memmove(active + position + 1u, active + position,
            (*active_count - position) * sizeof(*active));
    active[position] = cue_index;
    ++*active_count;
    return 1;
}

static int sami_active_remove(size_t *active, size_t *active_count,
                              size_t cue_index)
{
    size_t position = 0u;
    while (position < *active_count && active[position] != cue_index) ++position;
    if (position == *active_count) return 0;
    memmove(active + position, active + position + 1u,
            (*active_count - position - 1u) * sizeof(*active));
    --*active_count;
    return 1;
}

static int sami_write_document(const QwSubtitleDocument *document,
                               QwOutputFormat output_format,
                               const QwConvertOptions *options,
                               uint8_t **out_data, size_t *out_size,
                               QwConvertReport *report)
{
    static const char header[] =
        "<SAMI>\n<HEAD>\n<STYLE TYPE=\"text/css\">\n"
        "<!-- .ENCC { Name: Caption; SAMIType: CC; } -->\n"
        "</STYLE>\n</HEAD>\n<BODY>\n";
    static const char footer[] = "</BODY>\n</SAMI>\n";
    static const char empty_caption[] = "<P Class=ENCC>&nbsp;</P>\n";
    QwTextBuffer output = {0};
    SamiTransition *transitions = NULL;
    size_t *active = NULL;
    size_t transition_count;
    size_t active_count = 0u;
    size_t i;
    size_t overlap_events = 0u;
    int error = QW_STATUS_OK;
    (void)options;
    if (!document || output_format != QW_FORMAT_SAMI || !out_data || !out_size ||
        !qw_subtitle_document_validate(document)) return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    if (document->cue_count > SAMI_MAX_WRITER_CUES ||
        document->cue_count > SIZE_MAX / 2u ||
        document->cue_count * 2u > SIZE_MAX / sizeof(*transitions))
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    transition_count = document->cue_count * 2u;
    transitions = (SamiTransition *)malloc(transition_count * sizeof(*transitions));
    active = (size_t *)malloc(document->cue_count * sizeof(*active));
    if (!transitions || !active) {
        error = QW_STATUS_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (i = 0u; i < document->cue_count; ++i) {
        transitions[i * 2u].time = document->cues[i].start_milliseconds;
        transitions[i * 2u].cue_index = i;
        transitions[i * 2u].starts = 1;
        transitions[i * 2u + 1u].time = document->cues[i].end_milliseconds;
        transitions[i * 2u + 1u].cue_index = i;
        transitions[i * 2u + 1u].starts = 0;
    }
    qsort(transitions, transition_count, sizeof(*transitions),
          sami_transition_compare);
    output.limit = (size_t)SAMI_MAX_OUTPUT_BYTES + 1u;
    if (!sami_output_append(&output, header, sizeof(header) - 1u, &error))
        goto cleanup;
    if (transitions[0].time > 0u) {
        if (!sami_output_append(&output, "<SYNC Start=0>", 14u, &error) ||
            !sami_output_append(&output, empty_caption,
                                sizeof(empty_caption) - 1u, &error)) goto cleanup;
    }
    for (i = 0u; i < transition_count;) {
        size_t event_end = i;
        uint64_t time = transitions[i].time;
        while (event_end < transition_count && transitions[event_end].time == time) {
            int changed = transitions[event_end].starts
                ? sami_active_add(active, &active_count,
                                  transitions[event_end].cue_index)
                : sami_active_remove(active, &active_count,
                                     transitions[event_end].cue_index);
            if (!changed) {
                error = QW_STATUS_ERR_FORMAT;
                goto cleanup;
            }
            ++event_end;
        }
        if (!sami_output_append(&output, "<SYNC Start=", 12u, &error) ||
            !sami_append_time(&output, time, &error) ||
            !sami_output_append(&output, "><P Class=ENCC>", 15u, &error))
            goto cleanup;
        if (!active_count) {
            if (!sami_output_append(&output, "&nbsp;", 6u, &error)) goto cleanup;
        } else {
            size_t j;
            if (active_count > 1u) ++overlap_events;
            for (j = 0u; j < active_count; ++j) {
                const QwSubtitleCue *cue = &document->cues[active[j]];
                if (j && !sami_output_append(&output, "<br>", 4u, &error))
                    goto cleanup;
                if (!sami_escape_text(&output, cue->text_utf8, cue->text_size,
                                      &error)) goto cleanup;
            }
        }
        if (!sami_output_append(&output, "</P>\n", 5u, &error)) goto cleanup;
        i = event_end;
    }
    if (!sami_output_append(&output, footer, sizeof(footer) - 1u, &error))
        goto cleanup;
    *out_data = (uint8_t *)output.data;
    *out_size = output.size;
    output.data = NULL;
    if (report) {
        (void)snprintf(report->note, sizeof(report->note),
            "SAMI output keeps cue times and text in one ENCC class; source styles and cue metadata are omitted%s%s",
            overlap_events ? "; overlapping cues are joined with line breaks" : "",
            document->input_format == QW_FORMAT_LRC
                ? "; inferred cue ends are emitted as explicit SYNC boundaries" : "");
    }
    error = QW_STATUS_OK;

cleanup:
    free(output.data);
    free(transitions);
    free(active);
    return error;
}

const QwFormatDescriptor qw_format_sami = {
    .id = QW_FORMAT_SAMI,
    .name = "SAMI subtitles",
    .probe = sami_probe,
    .open_layout = sami_open_layout,
    .max_input_bytes = SAMI_MAX_INPUT_BYTES,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .open_subtitle = sami_open_subtitle
};

const QwSubtitleWriterDescriptor qw_writer_sami_subtitle = {
    QW_FORMAT_SAMI,
    "SAMI timed-cue writer",
    sami_write_document,
    SAMI_MAX_OUTPUT_BYTES
};
