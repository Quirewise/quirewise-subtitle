#include "subtitle_document.h"
#include "../text/html_entities.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Bounded TTML reader.
 *
 * The supported subset is the timed-text part of TTML 1/2: a <tt> root,
 * <p begin="..." end="..."|dur="..."> cues, nested <span> elements and
 * <br/> line breaks. Styling, regions, animation and metadata are ignored.
 * The input is never parsed as general XML; a tag scanner keeps the parser
 * independent from the shared XML package and bounds every allocation.
 */
enum {
    QW_TTML_MAX_INPUT_BYTES = 64u * 1024u * 1024u,
    QW_TTML_MAX_CUE_TEXT_BYTES = 1024u * 1024u,
    QW_TTML_DEFAULT_FRAME_RATE = 30u,
    QW_TTML_MAX_FRAME_RATE = 1000u
};

static int ttml_is_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static uint8_t ttml_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int ttml_slice_equal(const uint8_t *data, size_t size, const char *text)
{
    size_t index;
    size_t text_size = 0u;
    while (text[text_size]) ++text_size;
    if (size != text_size) return 0;
    for (index = 0u; index < size; ++index) {
        if (ttml_lower(data[index]) != ttml_lower((uint8_t)text[index]))
            return 0;
    }
    return 1;
}

static const uint8_t *ttml_local_name(const uint8_t *name, size_t size,
                                      size_t *out_size)
{
    const uint8_t *local = name;
    size_t index;
    for (index = 0u; index < size; ++index) {
        if (name[index] == ':') local = name + index + 1u;
    }
    if (out_size) *out_size = (size_t)(name + size - local);
    return local;
}

static int ttml_skip_until(const uint8_t *data, size_t size, size_t from,
                           const char *terminator, size_t *out_after)
{
    size_t term_size = strlen(terminator);
    size_t index;
    if (!data || !terminator || !out_after || from > size ||
        term_size > size - from) return 0;
    for (index = from; index + term_size <= size; ++index) {
        if (memcmp(data + index, terminator, term_size) == 0) {
            *out_after = index + term_size;
            return 1;
        }
    }
    return 0;
}

static int ttml_root_is_tt(const uint8_t *data, size_t size)
{
    size_t index = 0u;
    const uint8_t *name;
    size_t name_size = 0u;

    if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu && data[2] == 0xBFu)
        index = 3u;
    while (index < size && ttml_is_space(data[index])) ++index;
    while (index + 1u < size && data[index] == '<') {
        if (data[index + 1u] == '?') {
            if (!ttml_skip_until(data, size, index + 2u, "?>", &index))
                return 0;
        } else if (index + 3u < size && data[index + 1u] == '!' &&
                   data[index + 2u] == '-' && data[index + 3u] == '-') {
            if (!ttml_skip_until(data, size, index + 4u, "-->", &index))
                return 0;
        } else if (index + 8u < size && data[index + 1u] == '!' &&
                   ttml_slice_equal(data + index + 2u, 7u, "DOCTYPE")) {
            if (!ttml_skip_until(data, size, index + 2u, ">", &index))
                return 0;
        } else {
            break;
        }
        while (index < size && ttml_is_space(data[index])) ++index;
    }
    if (index >= size || data[index] != '<') return 0;
    ++index;
    if (index >= size || data[index] == '/' || data[index] == '!') return 0;
    name = data + index;
    while (index < size && !ttml_is_space(data[index]) && data[index] != '>' &&
           data[index] != '/') {
        ++index;
    }
    name_size = (size_t)(data + index - name);
    name = ttml_local_name(name, name_size, &name_size);
    return name_size == 2u && name[0] == 't' && name[1] == 't';
}

static int ttml_attr_get(const uint8_t *tag, size_t tag_size, const char *name,
                         const uint8_t **out_value, size_t *out_size)
{
    size_t index = 0u;

    /* Skip the element name. */
    while (index < tag_size && !ttml_is_space(tag[index]) &&
           tag[index] != '>' && tag[index] != '/') {
        ++index;
    }
    while (index < tag_size) {
        const uint8_t *name_start;
        const uint8_t *name_end;
        const uint8_t *value_start;
        const uint8_t *value_end;
        size_t local_size = 0u;
        const uint8_t *local;
        uint8_t quote = 0u;

        while (index < tag_size && ttml_is_space(tag[index])) ++index;
        if (index >= tag_size || tag[index] == '>' || tag[index] == '/') break;
        name_start = tag + index;
        while (index < tag_size && tag[index] != '=' &&
               !ttml_is_space(tag[index]) && tag[index] != '>' &&
               tag[index] != '/') {
            ++index;
        }
        name_end = tag + index;
        while (index < tag_size && ttml_is_space(tag[index])) ++index;
        if (index >= tag_size || tag[index] != '=') {
            /* Not a name=value pair; advance at least one byte. */
            if (index < tag_size) ++index;
            continue;
        }
        ++index;
        while (index < tag_size && ttml_is_space(tag[index])) ++index;
        if (index < tag_size && (tag[index] == '"' || tag[index] == '\'')) {
            quote = tag[index++];
        }
        value_start = tag + index;
        if (quote) {
            while (index < tag_size && tag[index] != quote) ++index;
        } else {
            while (index < tag_size && !ttml_is_space(tag[index]) &&
                   tag[index] != '>' && tag[index] != '/') {
                ++index;
            }
        }
        value_end = tag + index;
        if (quote && index < tag_size) ++index;
        local = ttml_local_name(name_start, (size_t)(name_end - name_start),
                                &local_size);
        if (ttml_slice_equal(local, local_size, name)) {
            if (out_value) *out_value = value_start;
            if (out_size) *out_size = (size_t)(value_end - value_start);
            return 1;
        }
    }
    return 0;
}

static int ttml_parse_unsigned(const uint8_t *data, size_t size,
                               uint64_t *out_value)
{
    uint64_t value = 0u;
    size_t index;
    if (!data || !size || !out_value) return 0;
    for (index = 0u; index < size; ++index) {
        if (data[index] < '0' || data[index] > '9') return 0;
        if (value > (UINT64_MAX - (uint64_t)(data[index] - '0')) / 10u) return 0;
        value = value * 10u + (uint64_t)(data[index] - '0');
    }
    *out_value = value;
    return 1;
}

static uint64_t ttml_fraction_to_ms(uint64_t fraction, size_t digits)
{
    while (digits < 3u) {
        fraction *= 10u;
        ++digits;
    }
    return fraction;
}

static int ttml_parse_clock(const uint8_t *text, size_t size,
                            unsigned frame_rate, uint64_t *out_ms)
{
    uint64_t parts[4] = {0u, 0u, 0u, 0u};
    size_t part_count = 0u;
    size_t index = 0u;
    uint64_t total = 0u;
    uint64_t fraction = 0u;
    size_t fraction_digits = 0u;
    int has_fraction = 0;

    while (index < size && part_count < 4u) {
        size_t start = index;
        while (index < size && text[index] != ':' && text[index] != '.' &&
               text[index] != ',') {
            ++index;
        }
        if (!ttml_parse_unsigned(text + start, index - start, &parts[part_count]))
            return 0;
        ++part_count;
        if (index < size && (text[index] == '.' || text[index] == ',')) {
            ++index;
            has_fraction = 1;
            while (index < size && text[index] >= '0' && text[index] <= '9' &&
                   fraction_digits < 3u) {
                fraction = fraction * 10u + (uint64_t)(text[index] - '0');
                ++fraction_digits;
                ++index;
            }
            while (index < size && text[index] >= '0' && text[index] <= '9')
                ++index;
            break;
        }
        if (index < size && text[index] == ':') {
            ++index;
            continue;
        }
        break;
    }
    if (index != size || part_count < 2u || part_count > 4u) return 0;
    if (part_count == 4u && has_fraction) return 0;
    if (part_count >= 2u && parts[1] >= 60u) return 0;
    if (part_count >= 3u && parts[2] >= 60u) return 0;
    if (part_count == 4u && parts[3] >= frame_rate) return 0;
    if (parts[0] > 100000u) return 0;
    total = parts[0] * 3600000u;
    if (part_count >= 2u) total += parts[1] * 60000u;
    if (part_count >= 3u) total += parts[2] * 1000u;
    if (part_count == 4u) total += (parts[3] * 1000u) / frame_rate;
    if (has_fraction) {
        total += ttml_fraction_to_ms(fraction, fraction_digits);
    }
    *out_ms = total;
    return 1;
}

static int ttml_parse_time(const uint8_t *text, size_t size,
                           unsigned frame_rate, uint64_t *out_ms)
{
    size_t start = 0u;
    size_t end = size;
    size_t index;
    int has_colon = 0;
    if (!text || !out_ms) return 0;
    while (start < end && ttml_is_space(text[start])) ++start;
    while (end > start && ttml_is_space(text[end - 1u])) --end;
    if (start == end) return 0;
    for (index = start; index < end; ++index) {
        if (text[index] == ':') has_colon = 1;
    }
    if (has_colon)
        return ttml_parse_clock(text + start, end - start, frame_rate, out_ms);
    {
        uint64_t value = 0u;
        uint64_t fraction = 0u;
        size_t fraction_digits = 0u;
        int has_fraction = 0;
        index = start;
        while (index < end && text[index] >= '0' && text[index] <= '9') {
            if (value > (UINT64_MAX - (uint64_t)(text[index] - '0')) / 10u)
                return 0;
            value = value * 10u + (uint64_t)(text[index] - '0');
            ++index;
        }
        if (index < end && (text[index] == '.' || text[index] == ',')) {
            ++index;
            has_fraction = 1;
            while (index < end && text[index] >= '0' && text[index] <= '9' &&
                   fraction_digits < 3u) {
                fraction = fraction * 10u + (uint64_t)(text[index] - '0');
                ++fraction_digits;
                ++index;
            }
            while (index < end && text[index] >= '0' && text[index] <= '9')
                ++index;
        }
        if (index < end) {
            size_t suffix = end - index;
            if (suffix == 2u && ttml_slice_equal(text + index, 2u, "ms")) {
                /* Fractional milliseconds are below the cue resolution. */
                *out_ms = value;
                return 1;
            }
            if (suffix == 1u && ttml_slice_equal(text + index, 1u, "s")) {
                if (value > UINT64_MAX / 1000u) return 0;
                *out_ms = value * 1000u;
                if (has_fraction)
                    *out_ms += ttml_fraction_to_ms(fraction, fraction_digits);
                return 1;
            }
            if (suffix == 1u && ttml_slice_equal(text + index, 1u, "m")) {
                if (value > UINT64_MAX / 60000u) return 0;
                *out_ms = value * 60000u;
                return 1;
            }
            if (suffix == 1u && ttml_slice_equal(text + index, 1u, "h")) {
                if (value > UINT64_MAX / 3600000u) return 0;
                *out_ms = value * 3600000u;
                return 1;
            }
            return 0;
        }
        if (value > UINT64_MAX / 1000u) return 0;
        *out_ms = value * 1000u;
        if (has_fraction)
            *out_ms += ttml_fraction_to_ms(fraction, fraction_digits);
        return 1;
    }
}

static unsigned ttml_frame_rate(const uint8_t *data, size_t size)
{
    /* Look at the first tag only; ttp:frameRate lives on the <tt> root. */
    size_t index = 0u;
    size_t end;
    const uint8_t *value;
    size_t value_size = 0u;
    uint64_t parsed = 0u;

    while (index < size && data[index] != '>') ++index;
    if (index >= size) return QW_TTML_DEFAULT_FRAME_RATE;
    end = index;
    if (ttml_attr_get(data, end, "frameRate", &value, &value_size) &&
        ttml_parse_unsigned(value, value_size, &parsed) && parsed > 0u &&
        parsed <= QW_TTML_MAX_FRAME_RATE) {
        return (unsigned)parsed;
    }
    return QW_TTML_DEFAULT_FRAME_RATE;
}

static int ttml_append_codepoint(uint8_t *buffer, size_t capacity, size_t *used,
                                 uint32_t codepoint)
{
    size_t needed;
    if (codepoint < 0x80u) needed = 1u;
    else if (codepoint < 0x800u) needed = 2u;
    else if (codepoint < 0x10000u) needed = 3u;
    else needed = 4u;
    if (*used + needed > capacity) return 0;
    if (needed == 1u) {
        buffer[(*used)++] = (uint8_t)codepoint;
    } else if (needed == 2u) {
        buffer[(*used)++] = (uint8_t)(0xC0u | (codepoint >> 6));
        buffer[(*used)++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    } else if (needed == 3u) {
        buffer[(*used)++] = (uint8_t)(0xE0u | (codepoint >> 12));
        buffer[(*used)++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
        buffer[(*used)++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    } else {
        buffer[(*used)++] = (uint8_t)(0xF0u | (codepoint >> 18));
        buffer[(*used)++] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3Fu));
        buffer[(*used)++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
        buffer[(*used)++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    }
    return 1;
}

static int ttml_text_reserve(uint8_t **buffer, size_t *capacity, size_t needed)
{
    size_t next;
    uint8_t *grown;
    if (needed <= *capacity) return 1;
    if (needed > QW_TTML_MAX_CUE_TEXT_BYTES) return 0;
    next = *capacity ? *capacity : 256u;
    while (next < needed) {
        if (next > QW_TTML_MAX_CUE_TEXT_BYTES / 2u) {
            next = QW_TTML_MAX_CUE_TEXT_BYTES;
            break;
        }
        next *= 2u;
    }
    if (next < needed) return 0;
    grown = (uint8_t *)realloc(*buffer, next);
    if (!grown) return 0;
    *buffer = grown;
    *capacity = next;
    return 1;
}

static int ttml_collect_cue_text(const uint8_t *data, size_t size,
                                 size_t from, size_t *out_end,
                                 char **out_text, size_t *out_text_size)
{
    uint8_t *buffer;
    size_t capacity = 0u;
    size_t used = 0u;
    size_t index = from;
    int pending_space = 0;

    if (!ttml_text_reserve(&buffer, &capacity, 256u)) return 0;
    while (index < size) {
        if (data[index] == '<') {
            size_t tag_end;
            size_t name_start;
            size_t name_size;
            const uint8_t *local;
            int closing = 0;

            if (index + 3u < size && data[index + 1u] == '!' &&
                data[index + 2u] == '-' && data[index + 3u] == '-') {
                if (!ttml_skip_until(data, size, index + 4u, "-->", &tag_end)) {
                    free(buffer);
                    return 0;
                }
                index = tag_end;
                continue;
            }
            if (index + 8u < size &&
                memcmp(data + index, "<![CDATA[", 9u) == 0) {
                size_t cdata_end;
                size_t cursor;
                if (!ttml_skip_until(data, size, index + 9u, "]]>", &cdata_end)) {
                    free(buffer);
                    return 0;
                }
                cursor = index + 9u;
                while (cursor + 3u < cdata_end) {
                    if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
                        free(buffer);
                        return 0;
                    }
                    buffer[used++] = data[cursor++];
                }
                index = cdata_end;
                continue;
            }
            if (index + 1u < size && data[index + 1u] == '/') {
                closing = 1;
                ++index;
            }
            ++index;
            name_start = index;
            while (index < size && !ttml_is_space(data[index]) &&
                   data[index] != '>' && data[index] != '/') {
                ++index;
            }
            name_size = index - name_start;
            local = ttml_local_name(data + name_start, name_size, &name_size);
            while (index < size && data[index] != '>') {
                if (data[index] == '"' || data[index] == '\'') {
                    uint8_t quote = data[index++];
                    while (index < size && data[index] != quote) ++index;
                }
                if (index < size) ++index;
            }
            if (index < size) ++index;
            if (closing && name_size == 1u && local[0] == 'p') {
                *out_end = index;
                break;
            }
            if (!closing && name_size == 2u && local[0] == 'b' &&
                local[1] == 'r') {
                if (used > 0u && buffer[used - 1u] != '\n') {
                    if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
                        free(buffer);
                        return 0;
                    }
                    buffer[used++] = '\n';
                }
                pending_space = 0;
            }
            continue;
        }
        if (data[index] == '&') {
            uint32_t codepoint = 0u;
            size_t consumed = 0u;
            if (qw_html_decode_entity(data, size, index, &codepoint, &consumed)) {
                if (pending_space && used > 0u && buffer[used - 1u] != '\n' &&
                    buffer[used - 1u] != ' ') {
                    if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
                        free(buffer);
                        return 0;
                    }
                    buffer[used++] = ' ';
                }
                pending_space = 0;
                if (!ttml_text_reserve(&buffer, &capacity, used + 4u) ||
                    !ttml_append_codepoint(buffer, capacity, &used, codepoint)) {
                    free(buffer);
                    return 0;
                }
                index += consumed;
                continue;
            }
        }
        if (ttml_is_space(data[index])) {
            pending_space = 1;
            ++index;
            continue;
        }
        if (pending_space && used > 0u && buffer[used - 1u] != '\n' &&
            buffer[used - 1u] != ' ') {
            if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
                free(buffer);
                return 0;
            }
            buffer[used++] = ' ';
        }
        pending_space = 0;
        if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
            free(buffer);
            return 0;
        }
        buffer[used++] = data[index++];
    }
    if (index >= size) {
        free(buffer);
        return 0;
    }
    while (used > 0u && (buffer[used - 1u] == ' ' || buffer[used - 1u] == '\n'))
        --used;
    if (!used) {
        free(buffer);
        return 0;
    }
    if (!ttml_text_reserve(&buffer, &capacity, used + 1u)) {
        free(buffer);
        return 0;
    }
    buffer[used] = '\0';
    *out_text = (char *)buffer;
    *out_text_size = used;
    return 1;
}

int qw_subtitle_parse_ttml_document(const void *input, size_t input_size,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report);

int qw_subtitle_parse_ttml_document(const void *input, size_t input_size,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t index = 0u;
    size_t cue_count = 0u;
    size_t skipped_count = 0u;
    unsigned frame_rate;
    int rc = QW_STATUS_ERR_FORMAT;

    if (!data || !input_size || !out_document)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    if (input_size > QW_TTML_MAX_INPUT_BYTES) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "TTML input exceeds the 64 MiB limit");
        return QW_STATUS_ERR_RESOURCE_LIMIT;
    }
    if (!ttml_root_is_tt(data, input_size)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "input is not a TTML document");
        return QW_STATUS_ERR_UNSUPPORTED;
    }
    frame_rate = ttml_frame_rate(data, input_size);
    qw_subtitle_document_init(out_document, QW_FORMAT_TTML);
    while (index < input_size) {
        size_t tag_start;
        size_t tag_end;
        size_t name_start;
        size_t name_size;
        const uint8_t *local;
        const uint8_t *value;
        size_t value_size = 0u;
        uint64_t begin_ms;
        uint64_t end_ms = 0u;
        uint64_t duration_ms = 0u;
        char *text = NULL;
        size_t text_size = 0u;

        while (index < input_size && data[index] != '<') ++index;
        if (index >= input_size) break;
        if (index + 3u < input_size && data[index + 1u] == '!' &&
            data[index + 2u] == '-' && data[index + 3u] == '-') {
            if (!ttml_skip_until(data, input_size, index + 4u, "-->", &index))
                break;
            continue;
        }
        if (index + 1u < input_size && data[index + 1u] == '?') {
            if (!ttml_skip_until(data, input_size, index + 2u, "?>", &index))
                break;
            continue;
        }
        tag_start = index + 1u;
        if (tag_start < input_size && data[tag_start] == '/') {
            if (!ttml_skip_until(data, input_size, tag_start, ">", &index))
                break;
            continue;
        }
        if (tag_start >= input_size) break;
        tag_end = tag_start;
        while (tag_end < input_size && data[tag_end] != '>') {
            if (data[tag_end] == '"' || data[tag_end] == '\'') {
                uint8_t quote = data[tag_end++];
                while (tag_end < input_size && data[tag_end] != quote)
                    ++tag_end;
            }
            if (tag_end < input_size) ++tag_end;
        }
        if (tag_end >= input_size) break;
        name_start = tag_start;
        {
            size_t cursor = tag_start;
            while (cursor < tag_end && !ttml_is_space(data[cursor]) &&
                   data[cursor] != '>' && data[cursor] != '/') {
                ++cursor;
            }
            name_size = cursor - name_start;
        }
        local = ttml_local_name(data + name_start, name_size, &name_size);
        if (!(name_size == 1u && local[0] == 'p')) {
            index = tag_end + 1u;
            continue;
        }
        /* Self-closing <p/> has no text. */
        if (tag_end > tag_start && data[tag_end - 1u] == '/') {
            index = tag_end + 1u;
            ++skipped_count;
            continue;
        }
        if (!ttml_attr_get(data + tag_start, tag_end - tag_start, "begin",
                           &value, &value_size) ||
            !ttml_parse_time(value, value_size, frame_rate, &begin_ms)) {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "TTML cue has no valid begin time");
            rc = QW_STATUS_ERR_FORMAT;
            goto fail;
        }
        if (ttml_attr_get(data + tag_start, tag_end - tag_start, "end",
                          &value, &value_size)) {
            if (!ttml_parse_time(value, value_size, frame_rate, &end_ms)) {
                if (report) snprintf(report->note, sizeof(report->note),
                                     "TTML cue has an invalid end time");
                rc = QW_STATUS_ERR_FORMAT;
                goto fail;
            }
        } else if (ttml_attr_get(data + tag_start, tag_end - tag_start, "dur",
                                 &value, &value_size)) {
            if (!ttml_parse_time(value, value_size, frame_rate, &duration_ms) ||
                duration_ms == 0u || begin_ms > UINT64_MAX - duration_ms) {
                if (report) snprintf(report->note, sizeof(report->note),
                                     "TTML cue has an invalid duration");
                rc = QW_STATUS_ERR_FORMAT;
                goto fail;
            }
            end_ms = begin_ms + duration_ms;
        } else {
            if (report) snprintf(report->note, sizeof(report->note),
                                 "TTML cue has neither end nor dur");
            rc = QW_STATUS_ERR_FORMAT;
            goto fail;
        }
        if (end_ms <= begin_ms) {
            size_t after = 0u;
            ++skipped_count;
            if (ttml_skip_until(data, input_size, tag_end + 1u, "</p", &after)) {
                index = after;
                while (index < input_size && data[index] != '>') ++index;
                if (index < input_size) ++index;
            } else {
                index = tag_end + 1u;
            }
            continue;
        }
        if (!ttml_collect_cue_text(data, input_size, tag_end + 1u, &index,
                                   &text, &text_size)) {
            size_t after = 0u;
            ++skipped_count;
            if (ttml_skip_until(data, input_size, tag_end + 1u, "</p", &after)) {
                index = after;
                while (index < input_size && data[index] != '>') ++index;
                if (index < input_size) ++index;
            } else {
                index = tag_end + 1u;
            }
            continue;
        }
        {
            const uint8_t *id_value = NULL;
            size_t id_size = 0u;
            if (!ttml_attr_get(data + tag_start, tag_end - tag_start, "id",
                               &id_value, &id_size)) {
                id_value = NULL;
                id_size = 0u;
            }
            if (!qw_subtitle_document_add_cue(
                    out_document, begin_ms, end_ms, 1, id_value, id_size,
                    NULL, 0u, (const uint8_t *)text, text_size)) {
                free(text);
                if (report) snprintf(report->note, sizeof(report->note),
                                     "TTML cue exceeds the subtitle limits");
                rc = QW_STATUS_ERR_RESOURCE_LIMIT;
                goto fail;
            }
        }
        free(text);
        ++cue_count;
    }
    if (!cue_count) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "TTML document contains no readable cues");
        rc = QW_STATUS_ERR_FORMAT;
        goto fail;
    }
    if (!qw_subtitle_document_validate(out_document)) {
        if (report) snprintf(report->note, sizeof(report->note),
                             "TTML cue model validation failed");
        rc = QW_STATUS_ERR_FORMAT;
        goto fail;
    }
    if (report) {
        if (skipped_count)
            snprintf(report->note, sizeof(report->note),
                     "TTML cues parsed: %lu (%lu skipped)",
                     (unsigned long)cue_count, (unsigned long)skipped_count);
        else
            snprintf(report->note, sizeof(report->note),
                     "TTML cues parsed: %lu", (unsigned long)cue_count);
        report->format = QW_FORMAT_TTML;
        report->pages_written = 0u;
    }
    return QW_STATUS_OK;

fail:
    qw_subtitle_document_dispose(out_document);
    return rc;
}
