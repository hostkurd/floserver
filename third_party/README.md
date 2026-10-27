# Third-party code

Header-only libraries copied in unchanged. Their licences ship with FloServer's
notices.

| Library | Version | Licence | Used for | SHA-256 of the header |
| --- | --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT (`nlohmann/LICENSE.MIT`) | floserver.json, the manifest, messages between the programs | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| [doctest](https://github.com/doctest/doctest) | 2.4.12 | MIT (`doctest/LICENSE.txt`) | unit tests only, not shipped | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |

To update one, replace the header with the release file, check its hash
against the release page, and update this table.
