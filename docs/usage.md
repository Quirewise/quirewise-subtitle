# quirewise-subtitle usage guide

This guide covers the task-level workflows of `Quirewise::Subtitle`. The public
API is declared in `quirewise/subtitle.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Consumer CMake:

```cmake
find_package(QuirewiseSubtitle CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Subtitle)
```

`QuirewiseSubtitle` depends on `QuirewiseRuntime`; install that package in the
same prefix first. There is no context object and no global state to initialise.

## 2. Core model and ownership

`qw_subtitle_parse` turns one buffer into one `QwSubtitle` document:

| Object | Owner | Valid until |
|---|---|---|
| input `data` / `size` | caller | only for the duration of the call (cue strings are copied) |
| `QwSubtitle` | caller | released by `qw_subtitle_free` |
| `identifier`, `settings`, `text` in `QwSubtitleCueView` | document | `qw_subtitle_free` |

A cue is addressed by a zero-based index. `QwSubtitleCueView` is a *view*: its
pointers are valid only while the document is alive.

`QwSubtitleInfo` summarizes the document:

- `input_format` is the parsed dialect; `cue_count` the number of usable cues.
- `frame_rate_num` / `frame_rate_den` give MicroDVD fps as a reduced rational,
  both `0` for every other format.

Each cue carries `start_milliseconds`, `end_milliseconds`, `end_time_explicit`,
an optional `identifier` (SRT cue number, WebVTT cue id, TTML `xml:id`), optional
`settings` (the WebVTT text after the timestamps) and `text` (UTF-8, may contain
`\n`).

## 3. Recipe: parse a subtitle file

```c
#include <quirewise/subtitle.h>

#include <stdio.h>
#include <stdlib.h>

static QwSubtitle *load_subtitle(const char *path, QwSubtitleInputFormat format)
{
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *bytes;
    QwSubtitle *document = NULL;

    if (!file) return NULL;
    fseek(file, 0L, SEEK_END); length = ftell(file); rewind(file);
    bytes = length > 0L ? malloc((size_t)length) : NULL;
    if (bytes && fread(bytes, 1u, (size_t)length, file) == (size_t)length) {
        if (qw_subtitle_parse(bytes, (size_t)length, format, &document) !=
            QW_SUBTITLE_OK) {
            document = NULL;
        }
    }
    free(bytes); /* safe: the parser copied every cue string */
    fclose(file);
    return document;
}
```

Then walk the cues:

```c
QwSubtitleInfo info;
size_t i;

if (qw_subtitle_get_info(document, &info) == QW_SUBTITLE_OK) {
    printf("%s: %zu cue(s)\n", qw_subtitle_format_name(info.input_format),
           info.cue_count);
    for (i = 0u; i < info.cue_count; ++i) {
        QwSubtitleCueView cue;
        if (qw_subtitle_get_cue(document, i, &cue) != QW_SUBTITLE_OK) continue;
        printf("[%llu -> %llu] %.*s\n",
               (unsigned long long)cue.start_milliseconds,
               (unsigned long long)cue.end_milliseconds,
               (int)cue.text_size, cue.text);
    }
}
```

The parse call copies the input, so the byte buffer can be freed immediately
after it returns. The document keeps only the cue data it needs.

## 4. Recipe: normalize inferred cue ends

`end_time_explicit == 0` means the end time was derived, not read: LRC ends are
`start + 5000 ms`, and SAMI/SCC ends come from the next caption event (the final
cue gets a five-second tail). A display timeline usually clamps those inferred
ends against the next cue start:

```c
static uint64_t display_end(const QwSubtitle *document, size_t index,
                            size_t count)
{
    QwSubtitleCueView cue, next;

    if (qw_subtitle_get_cue(document, index, &cue) != QW_SUBTITLE_OK)
        return 0u;
    if (cue.end_time_explicit || index + 1u >= count)
        return cue.end_milliseconds;
    if (qw_subtitle_get_cue(document, index + 1u, &next) == QW_SUBTITLE_OK &&
        next.start_milliseconds < cue.end_milliseconds) {
        return next.start_milliseconds;
    }
    return cue.end_milliseconds;
}
```

Keep the original value when the exact end matters; the helper is a rendering
policy, not a correction of the source.

## 5. Recipe: work with MicroDVD frame rates

MicroDVD addresses cues by frame number. The parser converts frames to
milliseconds using the fps header from the first three non-empty lines, or
23.976 fps (2997/125) when there is none. `QwSubtitleInfo` exposes the rate that
was used; both fields are `0` for every other format, so guard before dividing:

```c
static uint64_t frame_of(uint64_t milliseconds, const QwSubtitleInfo *info)
{
    uint64_t denominator = 1000u * (uint64_t)info->frame_rate_den;

    if (!info->frame_rate_num || !info->frame_rate_den ||
        milliseconds > UINT64_MAX / info->frame_rate_num) {
        return 0u;
    }
    return (milliseconds * info->frame_rate_num) / denominator;
}
```

## 6. Recipe: keep WebVTT identifiers and settings

WebVTT cue ids and cue settings survive parsing verbatim; the rest of the file
(header metadata, `NOTE`, `STYLE`, `REGION`) is dropped.

```c
QwSubtitleCueView cue;

if (qw_subtitle_get_cue(document, 0u, &cue) == QW_SUBTITLE_OK) {
    if (cue.identifier_size)
        printf("id=%.*s\n", (int)cue.identifier_size, cue.identifier);
    if (cue.settings_size)
        printf("settings=%.*s\n", (int)cue.settings_size, cue.settings);
    /* text is the cue payload, NUL-terminated but use cue.text_size */
}
```

`settings` is the raw remainder of the timing line (for example
`line:90% align:start`); this package does not interpret positioning
directives. For ASS/SSA it carries `layer|ml|mr|mv|effect` values, and a
`|RAW:` suffix preserves the original override tags so ASS/SSA→ASS/SSA can
restore them.

## 6.1 Write a subtitle

`qw_subtitle_write(document, format, &data, &size, &report)` emits SRT,
WebVTT, LRC, MicroDVD, TTML, ASS, SSA, SAMI or SCC. TTML output contains cue
begin/end and escaped text, not styling/regions/metadata. Release the result
with `qw_subtitle_buffer_free`. A target that cannot represent a cue
feature drops it and describes the loss in `report.note`; the output buffer is
still valid.

## 7. Status handling

| Status | Value | Meaning |
|---|---|---|
| `QW_SUBTITLE_OK` | 0 | document parsed / call succeeded |
| `QW_SUBTITLE_INVALID_ARGUMENT` | 1 | null pointer, empty input, bad enum, bad cue index |
| `QW_SUBTITLE_UNSUPPORTED` | 2 | recognized but unsupported dialect or variant |
| `QW_SUBTITLE_FORMAT_ERROR` | 3 | malformed timing, no usable cues, invalid structure |
| `QW_SUBTITLE_DECODE_ERROR` | 4 | input or text is not valid UTF-8 |
| `QW_SUBTITLE_OUT_OF_MEMORY` | 5 | allocation failed |
| `QW_SUBTITLE_RESOURCE_LIMIT` | 6 | a cue/content/input cap was exceeded |
| `QW_SUBTITLE_INTERNAL_ERROR` | 7 | unexpected internal failure |

`QW_SUBTITLE_UNSUPPORTED` usually means "different dialect than the enum you
passed", for example a non-TTML XML document parsed as TTML. Do not retry the
same call; re-probe the format instead. A parse failure always leaves
`*out_document` as `NULL`.

## 8. Memory, lifetime and threading

- No global mutable state; static tables are `const` and
  `qw_subtitle_format_name` returns string literals.
- Each document is independent, so documents can be parsed and read
  concurrently on different threads.
- A single `QwSubtitle` is read-only after parsing; concurrent
  `qw_subtitle_get_cue` calls are safe as long as no thread frees it at the
  same time.
- `qw_subtitle_free(NULL)` is a no-op.
- Cue strings are NUL-terminated and embedded NULs are rejected at parse time,
  so `strlen(text) == text_size`; prefer the size fields anyway.

## 9. Limits and per-format notes

Hard limits enforced by the cue model:

- 64 MiB input. SRT, WebVTT and LRC exceed it with
  `QW_SUBTITLE_INVALID_ARGUMENT`; the other parsers use
  `QW_SUBTITLE_RESOURCE_LIMIT`.
- 1,000,000 cues, 64 MiB of total cue content, 1024 bytes per identifier and
  4096 bytes per settings string.
- Text must be valid UTF-8; empty cue text and `start >= end` are rejected.

Per-format behavior:

| Format | Notes |
|---|---|
| SRT | `HH:MM:SS,mmm`, optional identifier line, no settings; markup is kept as text |
| WebVTT | requires a `WEBVTT` header; accepts `HH:MM:SS.mmm` and `MM:SS.mmm`; settings kept verbatim; `NOTE`/`STYLE`/`REGION` dropped |
| LRC | `[mm:ss.xx]` timestamps, several per line produce several cues; metadata tags skipped; ends inferred as `start + 5 s` |
| MicroDVD | `{start}{end}text`, pipe becomes `\n`; fps header searched in the first three non-empty lines; default 23.976 |
| TTML | `<tt>` root, `begin` plus `end`/`dur`, clock, offset and frame times; `<br/>` becomes `\n`, nested `<span>` merged; styles/regions dropped; writer emits begin/end and escaped text only |
| ASS/SSA | `[Script Info]` version checked; `Dialogue:` fields read; `\N` becomes a newline, `\n` a space, `\h` a no-break space; speaker/layer/margins/effect and raw override tags preserved for ASS/SSA direct round-trip; vector drawings unsupported |
| SAMI | first caption class selected; `<SYNC Start=...>` timing; HTML flattened; inferred ends |
| SCC | `Scenarist_SCC V1.0` header required; CC1 pop-on subset; 33 ms frame quantum; row placement flattened |

There is no bitmap subtitle support (PGS, VobSub) and no font/color model.
Cross-format conversion flattens positioning and styling; only ASS/SSA→ASS/SSA
round-trip preserves the speaker, layer/margins/effect and raw override tags.

## 10. Related packages

- `Quirewise::Runtime` supplies `QwStatus` and `QwFormatId`, the vocabulary the
  parser layer uses internally.
- `Quirewise::Pipeline` links this package when a conversion route needs cue
  extraction.
- The application/CLI renders cue text through its own PDF text writer; that
  writer is not part of this public package.

## 11. License

MIT. See `LICENSE`.
