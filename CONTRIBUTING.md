# Contributing to bootloader

Thanks for taking the time to contribute. This project is part of the
[soccentric-stm32](https://github.com/soccentric-stm32) STM32F767ZI baseline and is released under
the MIT License.

## Before you start

- Read [README.md](README.md) for what bootloader does and which tooling it needs.
- Report security issues privately, following [SECURITY.md](SECURITY.md), and not
  in a public issue.
- By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Getting set up

The firmware build runs in the `stm32-dev` Docker image, so the only host
requirement is Docker (plus `make`). The host tools and the Python tests run on
the host with Python 3.10+. See [README.md](README.md).

## Building and testing

| Command | What it does |
|---|---|
| `make build` | Build the bootloader image |
| `make test` | Run the host, Python and protocol contract tests |
| `make lint` | Run the static analysis checks |
| `make package-hwlink` | Build the HWLINK host package |
| `make flash` | Flash BOOT to a connected NUCLEO-F767ZI |

Run the build and the linter before you open a pull request, and add tests when
you change behaviour that is covered by tests.

## Code style

- Follow the style of the surrounding code; do not reformat unrelated lines.
- Keep the existing formatting configuration (`.clang-format`, `.editorconfig`,
  `rustfmt.toml`) authoritative instead of arguing about taste.
- Prefer small, readable changes over clever ones: this code runs on a bare-metal
  target and is read by people porting it to other boards.
- Mark hardware-facing changes in the pull request description so they can be
  verified on a board.

## Branch and commit conventions

- Branch off `master` and use a short, descriptive kebab-case name, for example
  `fix-spi-timeout` or `add-imx708-autofocus`.
- Write imperative, present-tense commit subjects (`fix:`, `feat:`, `docs:`,
  `refactor:`, `test:`, `build:`), keep them under about 72 characters and explain
  the reason for the change in the body.
- One logical change per commit.

## Pull requests

- Explain what changed and why, and how you tested it (including the board and
  toolchain you used).
- Reference the issue you are fixing (`Fixes #123`).
- Keep the pull request focused; split unrelated changes into separate ones.
- The Jenkins pipeline in the repository must be green before the change
  is merged.

## Changelog and releases

- Record user-visible changes under `## [Unreleased]` in
  [CHANGELOG.md](CHANGELOG.md), following
  [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
- Versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html) and
  releases are tagged `vMAJOR.MINOR.PATCH`.

## License

By contributing you agree that your contributions are licensed under the
[MIT License](LICENSE), the same terms as the rest of the project.
