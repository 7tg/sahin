# Prospero BlackBox dependencies (inherited from ProsperoStore)

The app is GPL-3.0-or-later. Its pinned foundation code and assets retain their
original notices in [UI_NOTICES.md](UI_NOTICES.md) and the root
[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

Additional sources are pinned by commit and SHA-256 in
[STORE_SOURCES.json](STORE_SOURCES.json). Their full licenses are alongside the
vendored files:

| Library | Purpose | License |
| --- | --- | --- |
| Monocypher 4.0.2 | Ed25519 verification | BSD-2-Clause or CC0-1.0 |
| yyjson 0.10.0 | Bounded JSON parsing | MIT |
| miniz 3.0.2 | ZIP inspection and extraction | MIT |
| PicoSHA2 | SHA-256 | MIT |
| stb_image (UI foundation) | Bounded PNG decode | MIT or public domain, full license in third_party/stb/stb_image.h |
| Project Nayuki QR Code generator v1.8.0 (UI foundation) | QR app links | MIT, full license in assets/licenses/qrcodegen.txt |

Host previews use the host's Mesa/EGL and libcurl. Elevated console HTTPS
statically links the following PacBrew v0.40.2 libraries. Sandboxed browsing
uses the console's networking libraries. Proprietary system modules are not included.

| Library | License text shipped in assets/licenses |
| --- | --- |
| libcurl 8.18.0 | [curl license](../assets/licenses/curl.txt) |
| OpenSSL 3.5.2 | [Apache-2.0](../assets/licenses/openssl.txt) |
| zlib 1.3.2 | [zlib license](../assets/licenses/zlib.txt) |
| zstd 1.5.6 | [BSD-3-Clause](../assets/licenses/zstd.txt) |
| libpsl 0.21.5 | [MIT](../assets/licenses/libpsl.txt) |
| Public Suffix List data | [MPL-2.0](../assets/licenses/public-suffix-list.txt) |

[PACBREW_LICENSES.json](PACBREW_LICENSES.json) records upstream license sources,
Git blob identifiers and local SHA-256 hashes. The build pins and verifies the
PacBrew archive; its compatibility examples retain their GPL-3.0-or-later notices.
