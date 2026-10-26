#ifndef QUIREWISE_PAGE_DOCUMENT_INTERNAL_H
#define QUIREWISE_PAGE_DOCUMENT_INTERNAL_H

#include "quirewise/document.h"

/* Typed page artifact shared by decoders and writers. */
typedef struct QwPageDocument {
    QwDocumentKind kind;
    unsigned flags;
    const uint8_t *pdf_data;
    size_t pdf_size;
    size_t page_count;
    QwPageAcquireFn acquire_page;
    QwPageReleaseFn release_page;
    void *context;
    /* Non-NULL only when this typed artifact owns its provider context. */
    QwDocumentDisposeFn dispose_context;
} QwPageDocument;

int qw_page_document_from_document(const QwDocument *source,
                                      QwPageDocument *out_page_document);
int qw_page_document_validate(const QwPageDocument *document);
void qw_page_document_dispose(QwPageDocument *document);
int qw_page_document_acquire(const QwPageDocument *document,
                             size_t index,
                             QwPageImage *out_page);
void qw_page_document_release(const QwPageDocument *document,
                              QwPageImage *page);

#endif
