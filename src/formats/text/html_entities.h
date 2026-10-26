#ifndef QUIREWISE_HTML_ENTITIES_INTERNAL_H
#define QUIREWISE_HTML_ENTITIES_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

int qw_html_decode_entity(const uint8_t *data, size_t size, size_t offset,
                          uint32_t *out_codepoint, size_t *out_consumed);

#endif
