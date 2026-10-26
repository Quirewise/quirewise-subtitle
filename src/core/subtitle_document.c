#include "subtitle_document.h"

#include <stdlib.h>
#include <string.h>

static int subtitle_utf8_valid(const uint8_t *text, size_t size,
                               int allow_line_breaks)
{
    size_t position = 0u;
    if (!text && size) return 0;
    while (position < size) {
        uint8_t first = text[position++];
        size_t continuation_count;
        uint32_t codepoint;
        uint32_t minimum;
        size_t i;
        if (first < 0x80u) {
            if (first == 0u || (first < 0x20u &&
                !(allow_line_breaks && (first == '\n' || first == '\t')))) {
                return 0;
            }
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
            (codepoint < 0x20u && !(allow_line_breaks &&
             (codepoint == '\n' || codepoint == '\t')))) return 0;
    }
    return 1;
}

static uint8_t *subtitle_copy(const uint8_t *text, size_t size)
{
    uint8_t *copy;
    if ((!text && size) || size == SIZE_MAX) return NULL;
    copy = (uint8_t *)malloc(size + 1u);
    if (!copy) return NULL;
    if (size) memcpy(copy, text, size);
    copy[size] = 0u;
    return copy;
}

void qw_subtitle_document_init(QwSubtitleDocument *document,
                               QwInputFormat input_format)
{
    if (!document) return;
    memset(document, 0, sizeof(*document));
    document->input_format = input_format;
}

int qw_subtitle_document_add_cue(QwSubtitleDocument *document,
                                 uint64_t start_milliseconds,
                                 uint64_t end_milliseconds,
                                 int end_time_explicit,
                                 const uint8_t *identifier,
                                 size_t identifier_size,
                                 const uint8_t *settings,
                                 size_t settings_size,
                                 const uint8_t *text,
                                 size_t text_size)
{
    QwSubtitleCue *grown;
    QwSubtitleCue cue;
    size_t added_bytes;
    size_t capacity;
    if (!document || start_milliseconds >= end_milliseconds ||
        (end_time_explicit != 0 && end_time_explicit != 1) ||
        (!identifier && identifier_size) || (!settings && settings_size) ||
        !text || !text_size || identifier_size > QW_SUBTITLE_MAX_IDENTIFIER_BYTES ||
        settings_size > QW_SUBTITLE_MAX_SETTINGS_BYTES ||
        !subtitle_utf8_valid(identifier, identifier_size, 0) ||
        !subtitle_utf8_valid(settings, settings_size, 0) ||
        !subtitle_utf8_valid(text, text_size, 1) ||
        document->cue_count >= QW_SUBTITLE_MAX_CUES ||
        identifier_size > QW_SUBTITLE_MAX_CONTENT_BYTES - document->content_bytes ||
        settings_size > QW_SUBTITLE_MAX_CONTENT_BYTES -
                            document->content_bytes - identifier_size ||
        text_size > QW_SUBTITLE_MAX_CONTENT_BYTES - document->content_bytes -
                        identifier_size - settings_size) return 0;
    added_bytes = identifier_size + settings_size + text_size;
    memset(&cue, 0, sizeof(cue));
    cue.identifier_utf8 = subtitle_copy(identifier, identifier_size);
    cue.settings_utf8 = subtitle_copy(settings, settings_size);
    cue.text_utf8 = subtitle_copy(text, text_size);
    if (!cue.identifier_utf8 || !cue.settings_utf8 || !cue.text_utf8) {
        free(cue.identifier_utf8);
        free(cue.settings_utf8);
        free(cue.text_utf8);
        return 0;
    }
    if (document->cue_count == document->cue_capacity) {
        capacity = document->cue_capacity ? document->cue_capacity * 2u : 32u;
        if (capacity < document->cue_capacity ||
            capacity > QW_SUBTITLE_MAX_CUES ||
            capacity > SIZE_MAX / sizeof(*grown)) {
            free(cue.identifier_utf8);
            free(cue.settings_utf8);
            free(cue.text_utf8);
            return 0;
        }
        grown = (QwSubtitleCue *)realloc(document->cues,
                                         capacity * sizeof(*grown));
        if (!grown) {
            free(cue.identifier_utf8);
            free(cue.settings_utf8);
            free(cue.text_utf8);
            return 0;
        }
        document->cues = grown;
        document->cue_capacity = capacity;
    }
    cue.start_milliseconds = start_milliseconds;
    cue.end_milliseconds = end_milliseconds;
    cue.end_time_explicit = end_time_explicit;
    cue.identifier_size = identifier_size;
    cue.settings_size = settings_size;
    cue.text_size = text_size;
    document->cues[document->cue_count++] = cue;
    document->content_bytes += added_bytes;
    return 1;
}

int qw_subtitle_document_validate(const QwSubtitleDocument *document)
{
    size_t total_bytes = 0u;
    size_t i;
    if (!document ||
        (document->input_format != QW_FORMAT_SRT &&
         document->input_format != QW_FORMAT_VTT &&
         document->input_format != QW_FORMAT_LRC &&
         document->input_format != QW_FORMAT_SUB &&
         document->input_format != QW_FORMAT_TTML &&
         document->input_format != QW_FORMAT_ASS &&
         document->input_format != QW_FORMAT_SSA &&
         document->input_format != QW_FORMAT_SAMI &&
         document->input_format != QW_FORMAT_SCC) ||
        ((document->frame_rate_num == 0u) !=
         (document->frame_rate_den == 0u)) ||
        (document->input_format == QW_FORMAT_SUB &&
         (!document->frame_rate_num || !document->frame_rate_den ||
          document->frame_rate_den > 1000000u ||
          document->frame_rate_num <= 3u * document->frame_rate_den ||
          document->frame_rate_num >= 100u * document->frame_rate_den)) ||
        (document->input_format != QW_FORMAT_SUB &&
         (document->frame_rate_num || document->frame_rate_den)) ||
        !document->cues || !document->cue_count ||
        document->cue_count > QW_SUBTITLE_MAX_CUES ||
        document->content_bytes > QW_SUBTITLE_MAX_CONTENT_BYTES) return 0;
    for (i = 0u; i < document->cue_count; ++i) {
        const QwSubtitleCue *cue = &document->cues[i];
        if (cue->start_milliseconds >= cue->end_milliseconds ||
            (cue->end_time_explicit != 0 && cue->end_time_explicit != 1) ||
            (document->input_format == QW_FORMAT_LRC &&
             cue->end_time_explicit) ||
            (document->input_format != QW_FORMAT_LRC &&
             document->input_format != QW_FORMAT_SAMI &&
             document->input_format != QW_FORMAT_SCC &&
             !cue->end_time_explicit) ||
            !cue->text_utf8 || !cue->text_size ||
            (!cue->identifier_utf8 && cue->identifier_size) ||
            (!cue->settings_utf8 && cue->settings_size) ||
            cue->identifier_size > QW_SUBTITLE_MAX_IDENTIFIER_BYTES ||
            cue->settings_size > QW_SUBTITLE_MAX_SETTINGS_BYTES ||
            !subtitle_utf8_valid(cue->identifier_utf8,
                                 cue->identifier_size, 0) ||
            !subtitle_utf8_valid(cue->settings_utf8,
                                 cue->settings_size, 0) ||
            !subtitle_utf8_valid(cue->text_utf8, cue->text_size, 1) ||
            cue->identifier_size > QW_SUBTITLE_MAX_CONTENT_BYTES - total_bytes ||
            cue->settings_size > QW_SUBTITLE_MAX_CONTENT_BYTES - total_bytes -
                                     cue->identifier_size ||
            cue->text_size > QW_SUBTITLE_MAX_CONTENT_BYTES - total_bytes -
                                 cue->identifier_size - cue->settings_size) {
            return 0;
        }
        total_bytes += cue->identifier_size + cue->settings_size + cue->text_size;
    }
    return total_bytes == document->content_bytes;
}

void qw_subtitle_document_dispose(QwSubtitleDocument *document)
{
    size_t i;
    if (!document) return;
    for (i = 0u; i < document->cue_count; ++i) {
        free(document->cues[i].identifier_utf8);
        free(document->cues[i].settings_utf8);
        free(document->cues[i].text_utf8);
    }
    free(document->cues);
    memset(document, 0, sizeof(*document));
}
