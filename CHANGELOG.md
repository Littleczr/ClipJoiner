# Changelog

## v0.3

- **Split...** button (scissors icon), where Swap used to be. It opens a small window for clip 1: type a time (`15`, `15.5` or `0:15.5`), check the preview, press Split or Enter.
  - Writes `<name>_part1.mp4` and `<name>_part2.mp4` into the Save to folder (`_part1_2` / `_part2_2`… if taken; nothing is overwritten).
  - Frame-exact: the time snaps to the nearest frame. The preview shows that frame, which is the first frame of part 2. Part 1 ends on the frame before it. No frame is lost or duplicated.
  - Re-encodes at the same near-lossless settings as the crossfade.
  - The window stays open, so you can split again or press Show in folder.
- **Swap** removed (Clear and re-drag covers it).
- Release builds are now a single static `ClipJoiner.exe` (no wxWidgets DLLs needed).

Tested: a 305-frame Wan2GP clip split at 3.5 s gave part 1 = 105 frames (ends on source frame 104) and part 2 = 200 frames (starts on source frame 105, the previewed frame).

## v0.2

- **0.5s crossfade** checkbox. The video fades and the audio crossfades at the same time. The result is 0.5 s shorter than the two clips added together. The checkbox state is remembered.
- **Save to** folder with **Browse...**. Join no longer asks for a file name: it writes `<clip1>_joined.mp4` into that folder, then `_joined_2`, `_joined_3`… if the name is taken. The folder is remembered (first run: your Desktop).
- **Show in folder** moved to the status line.
- Fixed: the "Done" message was immediately replaced by "Ready".
- Fixed: build errors under MSVC's default conformance settings.

Tested: two 10.17 s Wan2GP clips with crossfade → 19.83 s, 595 frames (305 + 305 − 15), audio 19.833 s; the same clips without crossfade → lossless 20.35 s; a Wan2GP clip plus a 720×1156 Opus clip with crossfade → 768×1216 at 30 fps, 20.47 s, audio in sync.

## v0.1

- Drag and drop two clips and join them back to back.
