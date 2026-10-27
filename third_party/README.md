# Third-party code

Libraries copied in unchanged. Their licences ship with FloServer's
notices.

| Library | Version | Licence | Used for | SHA-256 of the source |
| --- | --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT (`nlohmann/LICENSE.MIT`) | floserver.json, the manifest, messages between the programs | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| [Monocypher](https://monocypher.org) | 4.0.2 | BSD-2-Clause or CC0 (`monocypher/LICENCE.md`) | Ed25519 check of the signed manifest (D5); `monocypher.c` and `optional/monocypher-ed25519.c` | `monocypher.c` `02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d`, `monocypher-ed25519.c` `97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b` |
| [doctest](https://github.com/doctest/doctest) | 2.4.12 | MIT (`doctest/LICENSE.txt`) | unit tests only, not shipped | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |

To update one, replace the header with the release file, check its hash
against the release page, and update this table.
