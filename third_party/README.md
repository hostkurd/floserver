# Third-party code

Libraries copied in unchanged. Their licences ship with FloServer's
notices.

| Library | Version | Licence | Used for | SHA-256 of the source |
| --- | --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | MIT (`nlohmann/LICENSE.MIT`) | floserver.json, the manifest, messages between the programs | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| [Monocypher](https://monocypher.org) | 4.0.2 | BSD-2-Clause or CC0 (`monocypher/LICENCE.md`) | Ed25519 check of the signed manifest (D5); `monocypher.c` and `optional/monocypher-ed25519.c` | `monocypher.c` `02174117935699d418443c75a558a287deb06ef8cf7c1adced61d9047d2f323d`, `monocypher-ed25519.c` `97d581639dfa72be08a6d57deb7d79b736be001cb416819cab196d22559d242b` |
| [miniz](https://github.com/richgel999/miniz) | 3.0.2 | MIT (`miniz/LICENSE`) | unpacking runtime .zip files (D5); `miniz.c` and `miniz.h` from the release zip `miniz-3.0.2.zip` (SHA-256 `ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`) | `miniz.c` `0fcdc9888cb3a29ca8f176bac087e5fe6c7258a6ab06b1c271c1e109a11d3740`, `miniz.h` `295d1a0041aea09609598c0f1f35c1977ca05ad662acbadcfdaac44c140af37b` |
| [doctest](https://github.com/doctest/doctest) | 2.4.12 | MIT (`doctest/LICENSE.txt`) | unit tests only, not shipped | `94029a7d32da24a56249658147dbd2b33ff0b9ed665295cbbaf19aafff5b0ced` |

To update one, replace the header with the release file, check its hash
against the release page, and update this table.
