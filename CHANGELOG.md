# Changelog

All notable changes to MakeMkvCraft are documented in this file.

## [0.2.0] - 2026-11-07

### Added

- Typed source and disc models for ISO images, folders, and optical drives.
- Physical DVD title identity (VTS, TTN, PGC, and angle) and Blu-ray playlist,
  clip, and PID identity.
- Explicit Matroska `DefaultTrack` and `ForcedTrack` controls for output tracks.
- Windows optical-drive selection with label, readiness, and refresh support.
- A prioritized technical roadmap under `docs/ROADMAP.md`.

### Changed

- Unified DVD ISO, `VIDEO_TS` folder, and optical-drive remuxing through the
  logical-title `libdvdnav` pipeline.
- Replaced display-string dispatch with typed media/source dispatch.
- Improved multi-PGC duration calculation and DVD angle validation.
- Made detected forced subtitles an editable output choice rather than an
  immutable disposition.
- Logged the selected Matroska dispositions for each mapped stream.

### Known limitations

- Blu-ray remuxing remains experimental.
- The project does not yet register an automated CTest suite.
- Official archives do not include `libdvdcss`; encrypted media may require a
  compatible component supplied independently where its use is lawful.

## [0.1.0] - 2026-11-06

- First public preview with a portable Windows x86-64 package.
- DVD/Blu-ray discovery, selectable tracks and chapters, Matroska stream copy,
  DVD SCR correction, progress reporting, and partial-output cleanup.

[0.2.0]: https://github.com/jrpetrolla/MakeMkvCraft/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/jrpetrolla/MakeMkvCraft/releases/tag/v0.1.0
