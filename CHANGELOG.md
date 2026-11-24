# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0] - 2026-11-19

### Added

- Dual-bank A/B bootloader using the STM32F7 dual-bank flash: two
  application slots on one NUCLEO-F767ZI
- Image update over the ST-LINK virtual COM port with the HWLINK protocol
- CRC32 verification of every image and trial boot of new images
- Automatic rollback to the last confirmed slot when the application does
  not confirm itself within three starts
- Host tools for building images, updating devices and factory provisioning
- Host, Python and protocol contract tests

[Unreleased]: https://github.com/soccentric-stm32/bootloader/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/soccentric-stm32/bootloader/releases/tag/v1.0.0
