# quirewise-subtitle

Parse SRT, WebVTT, LRC, MicroDVD, TTML, ASS/SSA, SAMI and SCC subtitles into one
timed-cue model, and write eight of the nine dialects back out.

`Quirewise::Subtitle` is an independent, MIT-licensed C11 library, installed as
the `QuirewiseSubtitle` CMake package (target `Quirewise::Subtitle`). It can be
consumed without the Quirewise application.

## What it does

- Parses nine subtitle dialects from an in-memory buffer: SubRip (SRT), WebVTT,
  LRC, MicroDVD (`.sub`), TTML/DXFP, ASS, SSA, SAMI and Scenarist SCC.
- Normalizes every cue to start/end milliseconds, an explicit-end flag, an
  optional identifier, optional format settings and UTF-8 text.
- Validates UTF-8 and rejects malformed timing instead of guessing.
- Reports the cue count and, for MicroDVD, the source frame rate as a reduced
  rational (`frame_rate_num` / `frame_rate_den`).
- Keeps source-specific detail where the shared model has a slot for it: SRT
  cue numbers, WebVTT cue identifiers and settings, TTML `xml:id`, ASS dialogue
  text with line breaks.
- Writes SRT, WebVTT, LRC, MicroDVD, ASS, SSA, SAMI and SCC from the parsed cue
  list with `qw_subtitle_write`; the malloc-owned buffer is released with
  `qw_subtitle_buffer_free`. Cue text round-trips; target formats that cannot
  express a source feature (per-cue settings, explicit end times, sub-second
  precision) drop it and say so in the report note.

It does **not** write TTML, render or play subtitles, and has no styling or
positioning model, no font or color handling, no bitmap (PGS/VobSub) support
and no video/audio processing. Source styles, positioning, speakers, layers,
margins, effects and most header metadata are flattened or dropped on the way
in, so writers cannot restore them.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- `Quirewise::Runtime` is a public dependency (shared status/format vocabulary).
- Install `QuirewiseRuntime` in the same prefix; the exported package asks for
  it with `find_dependency`.
- CTest is used for the test suite.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds use `-DQUIREWISE_BUILD_SHARED=ON`; for a non-default prefix, pass
`-DCMAKE_PREFIX_PATH` when configuring your own project.

## Use it from CMake

```cmake
find_package(QuirewiseSubtitle CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Subtitle)
```

## Quick start

```c
#include <quirewise/subtitle.h>

#include <stdio.h>

int main(void)
{
    static const char srt[] =
        "1\n"
        "00:00:01,000 --> 00:00:03,500\n"
        "Hello subtitle\n"
        "\n"
        "2\n"
        "00:00:04,000 --> 00:00:05,000\n"
        "Second cue\n";
    QwSubtitle *document = NULL;
    QwSubtitleInfo info;
    size_t i;

    if (qw_subtitle_parse(srt, sizeof srt - 1u, QW_SUBTITLE_INPUT_SRT,
                          &document) != QW_SUBTITLE_OK) {
        fputs("parse failed\n", stderr);
        return 1;
    }
    if (qw_subtitle_get_info(document, &info) != QW_SUBTITLE_OK) {
        qw_subtitle_free(document);
        return 1;
    }
    printf("%s: %zu cue(s)\n", qw_subtitle_format_name(info.input_format),
           info.cue_count);

    for (i = 0u; i < info.cue_count; ++i) {
        QwSubtitleCueView cue;
        if (qw_subtitle_get_cue(document, i, &cue) != QW_SUBTITLE_OK) continue;
        printf("%llu-%llu %.*s\n", (unsigned long long)cue.start_milliseconds,
               (unsigned long long)cue.end_milliseconds, (int)cue.text_size,
               cue.text);
    }
    qw_subtitle_free(document);
    return 0;
}
```

The format argument is explicit: the caller knows (or has probed) the dialect
before parsing.

## API overview

| Header | Purpose |
|---|---|
| `quirewise/subtitle.h` | parse, inspect and free a cue document |

| Task | Functions |
|---|---|
| Parse | `qw_subtitle_parse` |
| Document summary | `qw_subtitle_get_info` |
| Random cue access | `qw_subtitle_get_cue` |
| Release | `qw_subtitle_free` |
| Format names | `qw_subtitle_format_name` |

`QwSubtitle` is an opaque document. `QwSubtitleInfo` carries the input format,
cue count and MicroDVD frame rate. `QwSubtitleCueView` exposes one cue:
`start_milliseconds`, `end_milliseconds`, `end_time_explicit`, `identifier` /
`identifier_size`, `settings` / `settings_size`, `text` / `text_size`.

## Behavior and limits

- The parser **copies** every cue string, so the input buffer may be released as
  soon as `qw_subtitle_parse` returns.
- Cue strings are borrowed from the document until `qw_subtitle_free` and are
  NUL-terminated for convenience; always use the paired `*_size` fields, because
  text may contain embedded newlines.
- Cues are returned in source order. `index` is zero-based; an out-of-range
  index returns `QW_SUBTITLE_INVALID_ARGUMENT`.
- `end_time_explicit` is `1` when the source carried an end time (SRT, WebVTT,
  TTML, ASS/SSA, MicroDVD) and `0` when the end was inferred: LRC cue ends are
  `start + 5000 ms`, while SAMI and SCC derive ends from the next caption event
  and give the final cue a five-second tail.
- Text must be valid UTF-8; invalid bytes return `QW_SUBTITLE_DECODE_ERROR`.
  Cue text with invalid controls, empty text, zero-length cues and cues that
  exceed the limits are rejected or skipped; a document with no usable cues
  fails with `QW_SUBTITLE_FORMAT_ERROR`.
- Hard caps: 64 MiB of input, 1,000,000 cues, 64 MiB of total cue content,
  1024 bytes per identifier and 4096 bytes per settings string. Exceeding them
  returns `QW_SUBTITLE_RESOURCE_LIMIT` (the 64 MiB input pre-check reports
  `QW_SUBTITLE_INVALID_ARGUMENT` for SRT/WebVTT/LRC).
- Unsupported dialects or variants return `QW_SUBTITLE_UNSUPPORTED`, for
  example a non-TTML XML document or an ASS script whose version cannot be
  established.
- MicroDVD without an fps header assumes 23.976 fps (2997/125). TTML frame
  clocks default to 30 fps unless `ttp:frameRate` is present.

## Testing

`ctest` covers SRT cue timing and identifiers, TTML timing forms (`clock`,
`offset`, frames), span/`<br/>`/entity text handling, malformed-timing
rejection, and a fuzz smoke run over mutated SRT input. The external consumer
template also parses a CRLF SRT fixture through the installed API.

## Documentation

See [docs/usage.md](docs/usage.md) for recipes, per-format notes and lifetime
rules.

## License

MIT. See [LICENSE](LICENSE).
