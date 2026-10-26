#ifndef QUIREWISE_SUBTITLE_FORMAT_INTERNAL_H
#define QUIREWISE_SUBTITLE_FORMAT_INTERNAL_H

#include "../../core/format_registry.h"
#include "../../core/subtitle_document.h"
#include "../../core/writer_registry.h"

int qw_subtitle_document_render_layout(const QwSubtitleDocument *subtitle,
                                       QwLayoutDocument *out_document,
                                       QwConvertReport *report);

extern const QwSubtitleWriterDescriptor qw_writer_srt_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_vtt_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_lrc_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_sub_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ttml_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ass_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_ssa_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_sami_subtitle;
extern const QwSubtitleWriterDescriptor qw_writer_scc_subtitle;

#endif
