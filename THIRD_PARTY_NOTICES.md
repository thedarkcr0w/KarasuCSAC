# Third-party notices

CS2AC uses the following third-party software. Each project remains under its own license and copyright.

- [Metamod:Source](https://github.com/alliedmodders/metamod-source), pinned as a Git submodule. License text is included in `metamod-source/LICENSE.txt`.
- [Source 2 SDK](https://github.com/alliedmodders/hl2sdk), CS2 branch, pinned as a Git submodule. Individual SDK files retain their Valve and contributor notices.
- [AMBuild](https://github.com/alliedmodders/ambuild), downloaded at the commit pinned in the bootstrap scripts. It is licensed under the BSD 3-Clause License.
- [ClientCvarValue](https://github.com/komashchenko/ClientCvarValue/tree/c29ea6112d9de4f1c349417d22337ba36e1adbe4), adapted for CS2AC's player-setting checks. It is licensed under GNU GPL version 3. Its exact license text is included in `licenses/CLIENTCVARVALUE-GPL-3.0.txt`.
- [DynLibUtils](https://github.com/komashchenko/DynLibUtils/tree/5eb95475170becfcc64fd5d32d14ec2b76dcb6d4), used by the ClientCvarValue integration. It is licensed under the MIT License. Its exact license text is included in `licenses/DYNLIBUTILS-MIT.txt`.
- [CS2KZ](https://github.com/KZGlobalTeam/cs2kz-metamod/tree/a8e99af7fb510b1776a489192d053e3d5553c020), portions of whose movement-analysis code were modified for CS2AC in 2026. It is licensed under GNU AGPL version 3. Its exact license text is included in `licenses/CS2KZ-AGPL-3.0.txt`.
- [Funchook](https://github.com/kubo/funchook/tree/7cb8819594f0d586454011ab691fab4edb625068), vendored as headers and prebuilt x64 libraries. Funchook is licensed under GNU GPL version 2 or later with its documented linking exception. Its exact license text is included in `licenses/FUNCHOOK.txt`.
- [diStorm 3.5.2b](https://github.com/gdabah/distorm/tree/3.5.2b), included in the vendored Funchook libraries. It is licensed under the BSD 3-Clause License. Its exact license text is included in `licenses/DISTORM.txt`.
- [tinyformat](https://github.com/c42f/tinyformat), vendored as a header. It is licensed under the Boost Software License 1.0.
- [PicoSHA2](https://github.com/okdshin/PicoSHA2/tree/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29), used to verify automatic-update packages. It is licensed under the MIT License. Its exact license text is included in `licenses/PICOSHA2-MIT.txt`.
- [miniz](https://github.com/richgel999/miniz/tree/77d0dce8627735138c51770d1799a1ef48f2117d), used to unpack verified automatic-update packages on both operating systems. It is licensed under the MIT License. Its exact license text is included in `licenses/MINIZ-MIT.txt`.
- [Protocol Buffers](https://github.com/protocolbuffers/protobuf), supplied by the pinned Source 2 SDK and generated during the build. It is licensed under the BSD 3-Clause License.

The upstream links above identify the corresponding source used by the vendored or modified components. Release archives include the applicable license texts under `addons/cs2ac/licenses`.
