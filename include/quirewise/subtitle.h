#ifndef QUIREWISE_SUBTITLE_H
#define QUIREWISE_SUBTITLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum QwSubtitleStatus {
    QW_SUBTITLE_OK = 0,
    QW_SUBTITLE_INVALID_ARGUMENT = 1,
    QW_SUBTITLE_UNSUPPORTED = 2,
    QW_SUBTITLE_FORMAT_ERROR = 3,
    QW_SUBTITLE_DECODE_ERROR = 4,
    QW_SUBTITLE_OUT_OF_MEMORY = 5,
    QW_SUBTITLE_RESOURCE_LIMIT = 6,
    QW_SUBTITLE_INTERNAL_ERROR = 7
} QwSubtitleStatus;

typedef enum QwSubtitleInputFormat {
    QW_SUBTITLE_INPUT_SRT = 1,
    QW_SUBTITLE_INPUT_VTT,
    QW_SUBTITLE_INPUT_LRC,
    QW_SUBTITLE_INPUT_ASS,
    QW_SUBTITLE_INPUT_SSA,
    QW_SUBTITLE_INPUT_SAMI,
    QW_SUBTITLE_INPUT_MICRODVD,
    QW_SUBTITLE_INPUT_SCC
} QwSubtitleInputFormat;

typedef struct QwSubtitleInfo {
    QwSubtitleInputFormat input_format;
    size_t cue_count;
    /* MicroDVD frame rate, reduced as a rational; zero for other formats. */
    uint32_t frame_rate_num;
    uint32_t frame_rate_den;
} QwSubtitleInfo;

typedef struct QwSubtitleCueView {
    uint64_t start_milliseconds;
    uint64_t end_milliseconds;
    int end_time_explicit;
    const char *identifier;
    size_t identifier_size;
    const char *settings;
    size_t settings_size;
    const char *text;
    size_t text_size;
} QwSubtitleCueView;

typedef struct QwSubtitle QwSubtitle;

/* The parser copies cue contents; the input buffer may be released on return. */
QwSubtitleStatus qw_subtitle_parse(const void *data,
                                   size_t size,
                                   QwSubtitleInputFormat input_format,
                                   QwSubtitle **out_document);

QwSubtitleStatus qw_subtitle_get_info(const QwSubtitle *document,
                                      QwSubtitleInfo *out_info);

/* Cue strings are borrowed until qw_subtitle_free. */
QwSubtitleStatus qw_subtitle_get_cue(const QwSubtitle *document,
                                     size_t index,
                                     QwSubtitleCueView *out_cue);

void qw_subtitle_free(QwSubtitle *document);

const char *qw_subtitle_format_name(QwSubtitleInputFormat input_format);

#ifdef __cplusplus
}
#endif

#endif
