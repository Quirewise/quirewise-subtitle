# quirewise-subtitle

Standalone C11 timed-text subtitle parser.

CMake target: `Quirewise::Subtitle`. Public header: `quirewise/subtitle.h`.

## Capabilities

Parses SRT, WebVTT, LRC, MicroDVD, TTML, ASS/SSA, SAMI and SCC into a shared cue
model (start/end milliseconds, explicit-end flag, identifier, settings and
text). Formats keep their source-specific metadata where the model allows it.

## Limits

The package is **read-only**: there is no public subtitle writer or converter.
(The internal writer descriptors are not exported; CLI support is limited to
rendering cues as PDF text pages.) Source styles, positioning, speakers, layers,
effects and header metadata are flattened or dropped. MicroDVD without a frame
rate assumes 23.976 fps. TTML styles/regions and nested span timing are
flattened; SCC covers the CC1 pop-on subset. Input is capped at 64 MiB.

## Build

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
