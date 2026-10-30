<p align="center"><img src="assets/joiny-original.png" width="128" alt="Joiny, the ClipJoiner icon"></p>

# ClipJoiner

A tiny Windows app that joins two video clips, back to back or with a 0.5 s crossfade, and splits a clip in two at an exact frame. Made for the short clips that AI video tools such as [Wan2GP](https://github.com/deepbeepmeep/Wan2GP) produce, when a full video editor is overkill.

<!-- Add a screenshot or short GIF here, e.g. ![ClipJoiner](docs/screenshot.png) -->

## Features

- **Join two clips.** Drop a clip on each slot and click **Join clips**. If both clips have the same format (codec, size, frame rate, pixel format, audio), they are stream-copied: instant and lossless.
- **0.5 s crossfade.** Picture and sound fade together. The result is 0.5 s shorter than the two clips added together.
- **Mixed formats.** Clips with different sizes or frame rates still join: clip 2 is scaled and padded to match clip 1, and silence is added to a clip with no audio.
- **Frame-exact split.** **Split...** cuts clip 1 at a time you type (`15`, `15.5` or `0:15.5`). The time snaps to the nearest frame and a preview shows the first frame of part 2. No frame is lost or duplicated.
- **Never overwrites.** Output goes to your **Save to** folder as `<name>_joined.mp4` or `<name>_part1.mp4` / `_part2.mp4`, with `_2`, `_3`… added if a name is taken.
- **Audio stays in sync.** Every re-encode pads or trims each clip's audio to exactly its video length.

## Download

1. Download `ClipJoiner.exe` from the [Releases](../../releases) page. It is a single file; nothing to install.
2. Get FFmpeg, which ClipJoiner uses to do the actual video work. Download a Windows build from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) (the "essentials" build is enough) or [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases).
3. Put `ffmpeg.exe` and `ffprobe.exe` in the same folder as `ClipJoiner.exe`, or anywhere on your PATH.

If ClipJoiner can't find FFmpeg, it says so in its status line.

**Requirements:** Windows 10 or 11, 64-bit.

## Usage

- **Add clips:** drag a video onto a slot, click a slot to browse, or drag one or two files onto `ClipJoiner.exe` itself.
- **Join:** pick a **Save to** folder (the Desktop by default), tick **0.5s crossfade** if you want one, and click **Join clips**.
- **Split:** load the clip as clip 1, click **Split...**, type the time and press **Split** or Enter. The window stays open so you can split again.
- **Show in folder** opens the output in Explorer. **Clear** empties both slots.

## Quality

- Stream copy (matching formats, no crossfade) is lossless.
- Crossfades, splits and mixed-format joins re-encode with libx264 at CRF 17 (visually near-lossless) and AAC 192 kbps audio. A typical short clip takes a few seconds.
- For a crossfade, each clip must be longer than 1 s.

## Settings

ClipJoiner remembers the Save to folder and the crossfade checkbox in `HKCU\Software\ClipJoiner`. Delete that key to reset them.

## Building from source

Requirements: Visual Studio 2026 (v145 toolset) with the C++ desktop workload, and [vcpkg](https://github.com/microsoft/vcpkg) with MSBuild integration (`vcpkg integrate install`).

1. Clone this repo and open `ClipJoiner.vcxproj`.
2. Build **Release | x64**. vcpkg installs wxWidgets automatically from `vcpkg.json` (triplet `x64-windows-static`), and the build links it statically with the static runtime (`/MT`), so the result is one self-contained `ClipJoiner.exe`.
3. Copy `ffmpeg.exe` and `ffprobe.exe` next to the built executable to run it.

ClipJoiner calls the FFmpeg command-line tools; it does not link any FFmpeg libraries. Needs wxWidgets 3.1.6 or newer (vcpkg provides 3.2+).

## Credits

Built with [wxWidgets](https://www.wxwidgets.org/) and [FFmpeg](https://ffmpeg.org/) (not included; download it separately). Developed with [LlamaBoss](https://llamaboss.com).

See [CHANGELOG.md](CHANGELOG.md) for version history.

## License

[MIT](LICENSE)
