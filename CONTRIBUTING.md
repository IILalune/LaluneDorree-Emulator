# Contributing

Bug reports, documentation improvements, and pull requests are welcome. Please follow the
[community code of conduct](CODE_OF_CONDUCT.md). Report vulnerabilities using the
[security policy](SECURITY.md), rather than a public bug report.

## Bug reports

Include the app version, macOS version, Mac architecture, steps to reproduce, expected behavior,
and what actually happened. For controller issues, include the model and USB or Bluetooth connection.
For game compatibility issues, include the game's title, region, and game code when available.

Do not upload commercial ROMs, Nintendo BIOS files, keys, copyrighted game assets, or links to
unauthorized downloads. Remove personal information from logs and screenshots. Save files and
state files may contain personal information or game data; do not attach them without reviewing
their contents and redistribution rights.

## Pull requests

1. Fork the repository and create a focused branch from `main`.
2. Make one clear change and follow the existing code style and explanatory comments.
3. Build and check the behavior you changed. Describe the checks and their results in the PR.
4. Update relevant documentation. Explain the problem and how the change solves it.

Build on macOS with CMake and SDL2, as described in the README:

```sh
cmake -S . -B build
cmake --build build -j4
tools/run_tests.sh
```

The test runner produces screenshots in `build/test-output/`; inspect them for the test ROMs'
results. Running the script alone is not proof that all emulation tests passed. For save-state
changes, also compare an uninterrupted run with a save/load continuation. For UI or controller
changes, check the affected screens and inputs manually.

Do not include generated build files, personal settings, saves, or unrelated changes. Release
DMGs and version changes are normally handled by the maintainer during release preparation.
If you change the serialized state layout, update `GBA::kStateVersion` so incompatible states
are rejected.

## Licensing

Submit only material you have the right to contribute. Contributions to the project's own code
are provided under its [MIT License](LICENSE). Preserve existing copyright notices and any
third-party license requirements. AI-assisted contributions must meet the same review, testing,
and licensing expectations as any other contribution.
