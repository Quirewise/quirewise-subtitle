#include "quirewise/subtitle.h"

#include "quirewise/status.h"
#include "subtitle_document.h"
#include "quirewise/convert.h"

#include <stdlib.h>
#include <string.h>

int qw_subtitle_parse_text_document(const void *input, size_t input_size,
                                    QwInputFormat input_format,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report);
int qw_subtitle_parse_ass_document(const void *input, size_t input_size,
                                   QwInputFormat input_format,
                                   QwSubtitleDocument *out_document,
                                   QwConvertReport *report);
int qw_subtitle_parse_sami_document(const void *input, size_t input_size,
                                    QwSubtitleDocument *out_document,
                                    QwConvertReport *report);
int qw_subtitle_parse_microdvd_document(const void *input, size_t input_size,
                                        QwSubtitleDocument *out_document,
                                        QwConvertReport *report);
int qw_subtitle_parse_scc_document(const void *input, size_t input_size,
                                   QwSubtitleDocument *out_document,
                                   QwConvertReport *report);

struct QwSubtitle {
    QwSubtitleDocument internal;
};

static int subtitle_input_format_map(QwSubtitleInputFormat input_format,
                                     QwInputFormat *out_format)
{
    if (!out_format) return 0;
    switch (input_format) {
    case QW_SUBTITLE_INPUT_SRT: *out_format = QW_FORMAT_SRT; return 1;
    case QW_SUBTITLE_INPUT_VTT: *out_format = QW_FORMAT_VTT; return 1;
    case QW_SUBTITLE_INPUT_LRC: *out_format = QW_FORMAT_LRC; return 1;
    case QW_SUBTITLE_INPUT_ASS: *out_format = QW_FORMAT_ASS; return 1;
    case QW_SUBTITLE_INPUT_SSA: *out_format = QW_FORMAT_SSA; return 1;
    case QW_SUBTITLE_INPUT_SAMI: *out_format = QW_FORMAT_SAMI; return 1;
    case QW_SUBTITLE_INPUT_MICRODVD: *out_format = QW_FORMAT_SUB; return 1;
    case QW_SUBTITLE_INPUT_SCC: *out_format = QW_FORMAT_SCC; return 1;
    default: return 0;
    }
}

static QwSubtitleStatus subtitle_status_map(int rc)
{
    switch (rc) {
    case QW_STATUS_OK: return QW_SUBTITLE_OK;
    case QW_STATUS_ERR_INVALID_ARGUMENT: return QW_SUBTITLE_INVALID_ARGUMENT;
    case QW_STATUS_ERR_UNSUPPORTED: return QW_SUBTITLE_UNSUPPORTED;
    case QW_STATUS_ERR_FORMAT: return QW_SUBTITLE_FORMAT_ERROR;
    case QW_STATUS_ERR_DECODE: return QW_SUBTITLE_DECODE_ERROR;
    case QW_STATUS_ERR_OUT_OF_MEMORY: return QW_SUBTITLE_OUT_OF_MEMORY;
    case QW_STATUS_ERR_RESOURCE_LIMIT: return QW_SUBTITLE_RESOURCE_LIMIT;
    default: return QW_SUBTITLE_INTERNAL_ERROR;
    }
}

QwSubtitleStatus qw_subtitle_parse(const void *data,
                                   size_t size,
                                   QwSubtitleInputFormat input_format,
                                   QwSubtitle **out_document)
{
    QwSubtitle *document;
    QwInputFormat internal_format;
    int rc;

    if (out_document) *out_document = NULL;
    if (!data || !size || !out_document ||
        !subtitle_input_format_map(input_format, &internal_format)) {
        return QW_SUBTITLE_INVALID_ARGUMENT;
    }
    document = (QwSubtitle *)calloc(1u, sizeof(*document));
    if (!document) return QW_SUBTITLE_OUT_OF_MEMORY;

    switch (input_format) {
    case QW_SUBTITLE_INPUT_SRT:
    case QW_SUBTITLE_INPUT_VTT:
    case QW_SUBTITLE_INPUT_LRC:
        rc = qw_subtitle_parse_text_document(data, size, internal_format,
                                             &document->internal, NULL);
        break;
    case QW_SUBTITLE_INPUT_ASS:
    case QW_SUBTITLE_INPUT_SSA:
        rc = qw_subtitle_parse_ass_document(data, size, internal_format,
                                            &document->internal, NULL);
        break;
    case QW_SUBTITLE_INPUT_SAMI:
        rc = qw_subtitle_parse_sami_document(data, size,
                                             &document->internal, NULL);
        break;
    case QW_SUBTITLE_INPUT_MICRODVD:
        rc = qw_subtitle_parse_microdvd_document(data, size,
                                                 &document->internal, NULL);
        break;
    case QW_SUBTITLE_INPUT_SCC:
        rc = qw_subtitle_parse_scc_document(data, size,
                                            &document->internal, NULL);
        break;
    default:
        free(document);
        return QW_SUBTITLE_UNSUPPORTED;
    }
    if (rc != QW_STATUS_OK) {
        qw_subtitle_document_dispose(&document->internal);
        free(document);
        return subtitle_status_map(rc);
    }
    *out_document = document;
    return QW_SUBTITLE_OK;
}

QwSubtitleStatus qw_subtitle_get_info(const QwSubtitle *document,
                                      QwSubtitleInfo *out_info)
{
    if (!document || !out_info) return QW_SUBTITLE_INVALID_ARGUMENT;
    out_info->input_format = QW_SUBTITLE_INPUT_SRT;
    switch (document->internal.input_format) {
    case QW_FORMAT_SRT: out_info->input_format = QW_SUBTITLE_INPUT_SRT; break;
    case QW_FORMAT_VTT: out_info->input_format = QW_SUBTITLE_INPUT_VTT; break;
    case QW_FORMAT_LRC: out_info->input_format = QW_SUBTITLE_INPUT_LRC; break;
    case QW_FORMAT_ASS: out_info->input_format = QW_SUBTITLE_INPUT_ASS; break;
    case QW_FORMAT_SSA: out_info->input_format = QW_SUBTITLE_INPUT_SSA; break;
    case QW_FORMAT_SAMI: out_info->input_format = QW_SUBTITLE_INPUT_SAMI; break;
    case QW_FORMAT_SUB: out_info->input_format = QW_SUBTITLE_INPUT_MICRODVD; break;
    case QW_FORMAT_SCC: out_info->input_format = QW_SUBTITLE_INPUT_SCC; break;
    default: return QW_SUBTITLE_INTERNAL_ERROR;
    }
    out_info->cue_count = document->internal.cue_count;
    out_info->frame_rate_num = document->internal.frame_rate_num;
    out_info->frame_rate_den = document->internal.frame_rate_den;
    return QW_SUBTITLE_OK;
}

QwSubtitleStatus qw_subtitle_get_cue(const QwSubtitle *document,
                                     size_t index,
                                     QwSubtitleCueView *out_cue)
{
    const QwSubtitleCue *cue;
    if (!document || !out_cue) return QW_SUBTITLE_INVALID_ARGUMENT;
    if (index >= document->internal.cue_count) return QW_SUBTITLE_INVALID_ARGUMENT;
    cue = &document->internal.cues[index];
    out_cue->start_milliseconds = cue->start_milliseconds;
    out_cue->end_milliseconds = cue->end_milliseconds;
    out_cue->end_time_explicit = cue->end_time_explicit;
    out_cue->identifier = (const char *)cue->identifier_utf8;
    out_cue->identifier_size = cue->identifier_size;
    out_cue->settings = (const char *)cue->settings_utf8;
    out_cue->settings_size = cue->settings_size;
    out_cue->text = (const char *)cue->text_utf8;
    out_cue->text_size = cue->text_size;
    return QW_SUBTITLE_OK;
}

void qw_subtitle_free(QwSubtitle *document)
{
    if (!document) return;
    qw_subtitle_document_dispose(&document->internal);
    free(document);
}

const char *qw_subtitle_format_name(QwSubtitleInputFormat input_format)
{
    static const char *const names[] = {
        "unknown", "srt", "vtt", "lrc", "ass", "ssa", "sami",
        "microdvd", "scc"
    };
    if ((unsigned)input_format >= sizeof(names) / sizeof(names[0]))
        return "unknown";
    return names[(unsigned)input_format];
}
