#include "html_entities.h"

static uint8_t html_ascii_lower(uint8_t value)
{
    return value >= 'A' && value <= 'Z'
        ? (uint8_t)(value + ('a' - 'A')) : value;
}

static int html_slice_equal(const uint8_t *data, size_t size,
                            const char *text)
{
    size_t i;
    size_t text_size = 0u;
    while (text[text_size]) ++text_size;
    if (size != text_size) return 0;
    for (i = 0u; i < size; ++i) {
        uint8_t expected = (uint8_t)text[i];
        if (html_ascii_lower(data[i]) != html_ascii_lower(expected)) return 0;
    }
    return 1;
}

int qw_html_decode_entity(const uint8_t *data, size_t size, size_t offset,
                          uint32_t *out_codepoint, size_t *out_consumed)
{
    size_t end;
    uint32_t value = 0u;
    unsigned base = 10u;
    if (!data || !out_codepoint || !out_consumed || offset >= size ||
        data[offset] != '&') return 0;
    end = offset + 1u;
    while (end < size && end - offset <= 16u && data[end] != ';' &&
           data[end] != '<' && data[end] != '&' && data[end] != ' ' &&
           data[end] != '\n' && data[end] != '\r' && data[end] != '\t') ++end;
    if (end >= size || data[end] != ';') return 0;
    if (offset + 1u < end && data[offset + 1u] == '#') {
        size_t i = offset + 2u;
        if (i < end && (data[i] == 'x' || data[i] == 'X')) {
            base = 16u;
            ++i;
        }
        if (i == end) return 0;
        for (; i < end; ++i) {
            unsigned digit;
            uint8_t ch = data[i];
            if (ch >= '0' && ch <= '9') digit = (unsigned)(ch - '0');
            else if (base == 16u && ch >= 'a' && ch <= 'f')
                digit = (unsigned)(ch - 'a' + 10);
            else if (base == 16u && ch >= 'A' && ch <= 'F')
                digit = (unsigned)(ch - 'A' + 10);
            else return 0;
            if (digit >= base || value > (0x10ffffu - digit) / base) return 0;
            value = value * base + digit;
        }
    } else {
        const uint8_t *name = data + offset + 1u;
        size_t name_size = end - offset - 1u;
        if (html_slice_equal(name, name_size, "amp")) value = '&';
        else if (html_slice_equal(name, name_size, "lt")) value = '<';
        else if (html_slice_equal(name, name_size, "gt")) value = '>';
        else if (html_slice_equal(name, name_size, "quot")) value = '"';
        else if (html_slice_equal(name, name_size, "apos")) value = '\'';
        else if (html_slice_equal(name, name_size, "nbsp")) value = 0xa0u;
        else if (html_slice_equal(name, name_size, "ndash")) value = 0x2013u;
        else if (html_slice_equal(name, name_size, "mdash")) value = 0x2014u;
        else if (html_slice_equal(name, name_size, "hellip")) value = 0x2026u;
        else if (html_slice_equal(name, name_size, "ldquo")) value = 0x201cu;
        else if (html_slice_equal(name, name_size, "rdquo")) value = 0x201du;
        else if (html_slice_equal(name, name_size, "lsquo")) value = 0x2018u;
        else if (html_slice_equal(name, name_size, "rsquo")) value = 0x2019u;
        else if (html_slice_equal(name, name_size, "euro")) value = 0x20acu;
        else return 0;
    }
    if (!value || (value >= 0xd800u && value <= 0xdfffu)) value = 0xfffdu;
    *out_codepoint = value;
    *out_consumed = end - offset + 1u;
    return 1;
}
