/* Subtitle writer dispatch.
 *
 * Every timed-cue writer registers one descriptor; this unit keeps the ordered
 * table, resolves an output format and exposes the internal entry point the
 * document composition layer calls. Descriptors are defined by their format
 * modules and declared here because the shared contract header only publishes
 * the descriptor type and the lookup function.
 */

#include "writer_registry.h"

#include "subtitle_document.h"

#include <stddef.h>
#include <stdio.h>

extern const QwSubtitleWriterDescriptor qw_writer_srt_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_vtt_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_lrc_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_sub_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_sami_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_scc_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ttml_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ass_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ssa_subtitle;

static const QwSubtitleWriterDescriptor *const qw_subtitle_writer_table[] = {
    &qw_writer_srt_subtitle,
    &qw_writer_vtt_subtitle,
    &qw_writer_lrc_subtitle,
    &qw_writer_sub_subtitle,
    &qw_writer_sami_subtitle,
    &qw_writer_scc_subtitle,
    &qw_writer_ttml_subtitle,
    &qw_writer_ass_subtitle,
    &qw_writer_ssa_subtitle
};

#define QW_SUBTITLE_WRITER_COUNT \
    (sizeof(qw_subtitle_writer_table) / sizeof(qw_subtitle_writer_table[0]))

const QwSubtitleWriterDescriptor *qw_subtitle_writer_find(
    QwOutputFormat output_format)
{
    size_t index;

    for (index = 0u; index < QW_SUBTITLE_WRITER_COUNT; ++index) {
        if (qw_subtitle_writer_table[index]->output_format == output_format)
            return qw_subtitle_writer_table[index];
    }
    return NULL;
}

int qw_subtitle_writer_registry_validate(char *error, size_t error_capacity)
{
    size_t outer;

    if (error && error_capacity) error[0] = '\0';
    for (outer = 0u; outer < QW_SUBTITLE_WRITER_COUNT; ++outer) {
        const QwSubtitleWriterDescriptor *writer =
            qw_subtitle_writer_table[outer];
        size_t inner;

        if (!writer->name || !writer->name[0]) {
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "subtitle writer %zu has no name", outer);
            return 0;
        }
        if (!writer->write_document) {
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "%s has no write function", writer->name);
            return 0;
        }
        if (!writer->max_output_bytes) {
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "%s has no output limit", writer->name);
            return 0;
        }
        for (inner = outer + 1u; inner < QW_SUBTITLE_WRITER_COUNT; ++inner) {
            if (qw_subtitle_writer_table[inner]->output_format ==
                writer->output_format) {
                if (error && error_capacity)
                    snprintf(error, error_capacity, "%s is registered twice",
                             writer->name);
                return 0;
            }
        }
    }
    return 1;
}

int qw_write_subtitle_document_to_format(const QwSubtitleDocument *document,
                                         QwOutputFormat output_format,
                                         const QwConvertOptions *options,
                                         uint8_t **out_data, size_t *out_size,
                                         QwConvertReport *report)
{
    const QwSubtitleWriterDescriptor *writer;

    if (!document || !out_data || !out_size)
        return QW_STATUS_ERR_INVALID_ARGUMENT;
    *out_data = NULL;
    *out_size = 0u;
    writer = qw_subtitle_writer_find(output_format);
    if (!writer || !writer->write_document)
        return QW_STATUS_ERR_UNSUPPORTED;
    return writer->write_document(document, output_format, options, out_data,
                                  out_size, report);
}
