#ifndef QUIREWISE_WRITER_REGISTRY_INTERNAL_H
#define QUIREWISE_WRITER_REGISTRY_INTERNAL_H

#include "quirewise/convert.h"
#include "page_document.h"
#include "architecture_metadata.h"
#include "audio_document.h"
#include "layout_document.h"
#include "spreadsheet_document.h"
#include "data_document.h"

#include <stdio.h>

struct QwSubtitleDocument;

typedef int (*QwPageDocumentWriterFn)(const QwPageDocument *document,
                                      const QwConvertOptions *options,
                                      uint8_t **out_data,
                                      size_t *out_size,
                                      size_t *out_pages);
typedef int (*QwLayoutDocumentWriterFn)(const QwLayoutDocument *document,
                                        const QwConvertOptions *options,
                                        uint8_t **out_data,
                                        size_t *out_size,
                                        size_t *out_pages,
                                        QwConvertReport *report);
typedef int (*QwAudioDocumentWriterFn)(const QwAudioDocument *document,
                                       QwOutputFormat output_format,
                                       const QwConvertOptions *options,
                                       uint8_t **out_data,
                                       size_t *out_size,
                                       QwConvertReport *report);
typedef int (*QwAudioDocumentStreamWriterFn)(
    const QwAudioDocument *document,
    QwOutputFormat output_format,
    const QwConvertOptions *options,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    QwConvertReport *report);

typedef int (*QwPageDocumentStreamWriterFn)(
    const QwPageDocument *document,
    const QwConvertOptions *options,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    size_t *out_pages);

typedef int (*QwLayoutDocumentStreamWriterFn)(
    const QwLayoutDocument *document,
    const QwConvertOptions *options,
    FILE *stream,
    size_t max_output_bytes,
    size_t *out_size,
    size_t *out_pages,
    QwConvertReport *report);

typedef struct QwLayoutStreamWriterDescriptor {
    QwOutputFormat output_format;
    const char *name;
    QwLayoutDocumentStreamWriterFn write_document;
} QwLayoutStreamWriterDescriptor;

/* Optional streaming edges are kept separate to preserve the layout of the
 * existing writer descriptor used by format modules. */
typedef struct QwPageStreamWriterDescriptor {
    QwOutputFormat output_format;
    unsigned accepted_document_kinds;
    const char *name;
    QwPageDocumentStreamWriterFn write_document;
} QwPageStreamWriterDescriptor;

/* Audio formats share one FFmpeg muxer adapter for stream output. */
typedef struct QwAudioStreamWriterDescriptor {
    const char *name;
    QwAudioDocumentStreamWriterFn write_document;
} QwAudioStreamWriterDescriptor;

typedef struct QwSpreadsheetWriterDescriptor {
    QwOutputFormat output_format;
    const char *name;
    int (*write_document)(const struct QwSpreadsheetDocument *document,
                          QwOutputFormat output_format,
                          const QwConvertOptions *options,
                          uint8_t **out_data, size_t *out_size,
                          QwConvertReport *report);
    size_t max_output_bytes;
} QwSpreadsheetWriterDescriptor;

typedef struct QwSubtitleWriterDescriptor {
    QwOutputFormat output_format;
    const char *name;
    int (*write_document)(const struct QwSubtitleDocument *document,
                          QwOutputFormat output_format,
                          const QwConvertOptions *options,
                          uint8_t **out_data, size_t *out_size,
                          QwConvertReport *report);
    size_t max_output_bytes;
} QwSubtitleWriterDescriptor;

typedef struct QwDataWriterDescriptor {
    QwOutputFormat output_format;
    const char *name;
    int (*write_document)(const QwDataDocument *document,
                          QwOutputFormat output_format,
                          const QwConvertOptions *options,
                          uint8_t **out_data, size_t *out_size,
                          QwConvertReport *report);
    size_t max_output_bytes;
} QwDataWriterDescriptor;

typedef struct QwWriterDescriptor {
    QwOutputFormat output_format;
    unsigned accepted_document_kinds;
    unsigned cost;
    const char *name;
    QwPageDocumentWriterFn write_document;
    unsigned required_backends;
    size_t max_pages;
    unsigned accepted_media_kinds;
    QwLayoutDocumentWriterFn write_layout_document;
    QwAudioDocumentWriterFn write_audio_document;
    /* Zero uses the finite output limit assigned by the format profile. */
    size_t max_output_bytes;
    /* Zero means no declared cumulative raster-pixel limit. */
    size_t max_total_pixels;
} QwWriterDescriptor;

#define QW_DOCUMENT_KIND_BIT(kind) (1u << ((unsigned)(kind) - 1u))

extern const QwWriterDescriptor qw_writer_pdf;
extern const QwWriterDescriptor qw_writer_eml;
extern const QwWriterDescriptor qw_writer_txt;
extern const QwDataWriterDescriptor qw_data_json_writer;
extern const QwDataWriterDescriptor qw_data_jsonl_writer;
extern const QwDataWriterDescriptor qw_data_yaml_writer;
extern const QwDataWriterDescriptor qw_data_toml_writer;
extern const QwDataWriterDescriptor qw_data_xml_writer;
extern const QwDataWriterDescriptor qw_data_vcard_writer;
extern const QwSpreadsheetWriterDescriptor qw_spreadsheet_vcard_writer;

const QwDataWriterDescriptor *qw_data_writer_find(QwOutputFormat output_format);
int qw_data_writer_registry_validate(char *error, size_t error_capacity);
int qw_write_data_document_to_format(const QwDataDocument *document,
                                     QwOutputFormat output_format,
                                     const QwConvertOptions *options,
                                     uint8_t **out_data, size_t *out_size,
                                     QwConvertReport *report);
extern const QwWriterDescriptor qw_writer_webp;
extern const QwWriterDescriptor qw_writer_heic;
extern const QwPageStreamWriterDescriptor qw_stream_writer_heic;
extern const QwWriterDescriptor qw_writer_png;
extern const QwWriterDescriptor qw_writer_svg;
extern const QwWriterDescriptor qw_writer_svgz;
extern const QwWriterDescriptor qw_writer_bmp;
extern const QwWriterDescriptor qw_writer_qoi;
extern const QwWriterDescriptor qw_writer_wbmp;
extern const QwWriterDescriptor qw_writer_ppm;
extern const QwWriterDescriptor qw_writer_pam;
extern const QwWriterDescriptor qw_writer_xbm;
extern const QwWriterDescriptor qw_writer_ico;
extern const QwWriterDescriptor qw_writer_cur;
extern const QwWriterDescriptor qw_writer_icns;
extern const QwWriterDescriptor qw_writer_tga;
extern const QwWriterDescriptor qw_writer_pcx;
extern const QwWriterDescriptor qw_writer_tiff;
extern const QwWriterDescriptor qw_writer_jpg;
extern const QwWriterDescriptor qw_writer_ras;
extern const QwWriterDescriptor qw_writer_sgi;
extern const QwWriterDescriptor qw_writer_xpm;
extern const QwWriterDescriptor qw_writer_miff;
extern const QwWriterDescriptor qw_writer_gif;
extern const QwWriterDescriptor qw_writer_apng;
extern const QwWriterDescriptor qw_writer_mng;
extern const QwWriterDescriptor qw_writer_psd;
extern const QwWriterDescriptor qw_writer_jp2;
extern const QwWriterDescriptor qw_writer_avif;
extern const QwWriterDescriptor qw_writer_jxl;
extern const QwWriterDescriptor qw_writer_dds;
extern const QwWriterDescriptor qw_writer_hdr;
extern const QwWriterDescriptor qw_writer_cbz;
extern const QwWriterDescriptor qw_writer_cbt;
extern const QwWriterDescriptor qw_writer_pfm;
extern const QwWriterDescriptor qw_writer_psb;
extern const QwLayoutStreamWriterDescriptor qw_layout_stream_writer_eml;
extern const QwPageStreamWriterDescriptor qw_stream_writer_pdf;
extern const QwPageStreamWriterDescriptor qw_stream_writer_txt;
extern const QwLayoutStreamWriterDescriptor qw_layout_stream_writer_pdf;
extern const QwPageStreamWriterDescriptor qw_stream_writer_gif;
extern const QwPageStreamWriterDescriptor qw_stream_writer_apng;
extern const QwPageStreamWriterDescriptor qw_stream_writer_mng;
extern const QwPageStreamWriterDescriptor qw_stream_writer_psd;
extern const QwPageStreamWriterDescriptor qw_stream_writer_psb;
extern const QwPageStreamWriterDescriptor qw_stream_writer_ico;
extern const QwPageStreamWriterDescriptor qw_stream_writer_cur;
extern const QwPageStreamWriterDescriptor qw_stream_writer_icns;
extern const QwPageStreamWriterDescriptor qw_stream_writer_jp2;
extern const QwPageStreamWriterDescriptor qw_stream_writer_avif;
extern const QwPageStreamWriterDescriptor qw_stream_writer_jxl;
extern const QwPageStreamWriterDescriptor qw_stream_writer_jpg;
extern const QwPageStreamWriterDescriptor qw_stream_writer_webp;
extern const QwPageStreamWriterDescriptor qw_stream_writer_bmp;
extern const QwPageStreamWriterDescriptor qw_stream_writer_ppm;
extern const QwPageStreamWriterDescriptor qw_stream_writer_pam;
extern const QwPageStreamWriterDescriptor qw_stream_writer_qoi;
extern const QwPageStreamWriterDescriptor qw_stream_writer_wbmp;
extern const QwPageStreamWriterDescriptor qw_stream_writer_pfm;
extern const QwPageStreamWriterDescriptor qw_stream_writer_hdr;
extern const QwPageStreamWriterDescriptor qw_stream_writer_cbz;
extern const QwPageStreamWriterDescriptor qw_stream_writer_cbt;
extern const QwPageStreamWriterDescriptor qw_stream_writer_tga;
extern const QwPageStreamWriterDescriptor qw_stream_writer_ras;
extern const QwPageStreamWriterDescriptor qw_stream_writer_sgi;
extern const QwPageStreamWriterDescriptor qw_stream_writer_xbm;
extern const QwPageStreamWriterDescriptor qw_stream_writer_miff;
extern const QwPageStreamWriterDescriptor qw_stream_writer_png;
extern const QwPageStreamWriterDescriptor qw_stream_writer_svg;
extern const QwPageStreamWriterDescriptor qw_stream_writer_svgz;
extern const QwPageStreamWriterDescriptor qw_stream_writer_xpm;
extern const QwPageStreamWriterDescriptor qw_stream_writer_pcx;
extern const QwPageStreamWriterDescriptor qw_stream_writer_dds;
extern const QwPageStreamWriterDescriptor qw_stream_writer_tiff;

const QwWriterDescriptor *qw_writer_find(QwOutputFormat output_format,
                                         QwDocumentKind document_kind);
const QwWriterDescriptor *qw_writer_find_media(QwOutputFormat output_format,
                                               QwMediaKind media_kind);
int qw_write_layout_document_to_format(const QwLayoutDocument *document,
                                       QwOutputFormat output_format,
                                       const QwConvertOptions *options,
                                       uint8_t **out_data,
                                       size_t *out_size,
                                       QwConvertReport *report);
int qw_write_layout_document_to_stream(const QwLayoutDocument *document,
                                       QwOutputFormat output_format,
                                       const QwConvertOptions *options,
                                       FILE *stream,
                                       size_t max_output_bytes,
                                       size_t *out_size,
                                       size_t *out_pages,
                                       QwConvertReport *report);
int qw_write_audio_document_to_format(const QwAudioDocument *document,
                                      QwOutputFormat output_format,
                                      const QwConvertOptions *options,
                                      uint8_t **out_data,
                                      size_t *out_size,
                                      QwConvertReport *report);
int qw_write_audio_document_to_stream(const QwAudioDocument *document,
                                      QwOutputFormat output_format,
                                      const QwConvertOptions *options,
                                      FILE *stream,
                                      size_t max_output_bytes,
                                      size_t *out_size,
                                      QwConvertReport *report);
const QwSpreadsheetWriterDescriptor *qw_spreadsheet_writer_find(
    QwOutputFormat output_format);
int qw_spreadsheet_writer_registry_validate(char *error,
                                           size_t error_capacity);
int qw_write_spreadsheet_document_to_format(
    const struct QwSpreadsheetDocument *document,
    QwOutputFormat output_format, const QwConvertOptions *options,
    uint8_t **out_data, size_t *out_size, QwConvertReport *report);
const QwSubtitleWriterDescriptor *qw_subtitle_writer_find(
    QwOutputFormat output_format);
int qw_subtitle_writer_registry_validate(char *error, size_t error_capacity);
int qw_write_subtitle_document_to_format(
    const struct QwSubtitleDocument *document,
    QwOutputFormat output_format, const QwConvertOptions *options,
    uint8_t **out_data, size_t *out_size, QwConvertReport *report);
int qw_write_page_document_to_stream(const QwPageDocument *document,
                                    QwOutputFormat output_format,
                                    const QwConvertOptions *options,
                                    FILE *stream,
                                    size_t max_output_bytes,
                                    size_t *out_size,
                                    size_t *out_pages,
                                    QwConvertReport *report);
int qw_writer_has_page_stream(QwOutputFormat output_format);
int qw_writer_has_layout_stream(QwOutputFormat output_format);
int qw_writer_has_audio_stream(QwOutputFormat output_format);
size_t qw_writer_registry_count(void);
const QwWriterDescriptor *qw_writer_registry_at(size_t index);
int qw_writer_registry_validate(char *error, size_t error_capacity);
int qw_writer_validate_page_count(QwOutputFormat output_format,
                                  QwDocumentKind document_kind,
                                  size_t page_count,
                                  char *note,
                                  size_t note_capacity);
int qw_writer_validate_output_size(const QwWriterDescriptor *writer,
                                   size_t output_size,
                                   char *note,
                                   size_t note_capacity);
size_t qw_writer_output_limit(const QwWriterDescriptor *writer);
const char *qw_output_format_extension(QwOutputFormat output_format);

int qw_pdf_layout_write_document(const QwLayoutDocument *document,
                                 const QwConvertOptions *options,
                                 uint8_t **out_data,
                                 size_t *out_size,
                                 size_t *out_pages,
                                 QwConvertReport *report);

#endif
