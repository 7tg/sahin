# PacBrew curl in native titles

Compatibility functions adapted from BlackBearReloaded's ProsperoRadio curl
guide (2026-10-02, GPL-3.0-or-later). The guide reports console validation of
PacBrew v0.40.2, libcurl 8.18.0 and OpenSSL 3.5.2 in an elevated native title.
The pinned dependency bootstrapper already supports `PACBREW_PACKAGES=libcurl`.

Compile `compat.c` and `netdb.c` into the adopter. The DNS definitions prevent
null imports from the unloaded WebKit POSIX module. DNS supports IPv4, numeric
ports and one address per hostname, with a private resolver per lookup.
`openlog`, `popen` and `pclose` are weak so existing app implementations can provide them.

Use `CURLOPT_SOCKOPTFUNCTION` to set `SO_NBIO` (0x1200) on each socket and fail
if it cannot be set. Set `CURLOPT_NOSIGNAL=1`, HTTP/1.1 and an explicit CA file:
`/system/common/cert/CA_LIST.cer` after elevation. Keep hostname and peer
verification enabled. Check every option result. A sandboxed native app should
keep its `sceHttp` path: the guide observed BSD connection failures there.

Run `make test-pacbrew-curl` for UTC calendar and resolver checks. Every adopter
must still qualify its final linked native build on a console. Do not ship the
PacBrew archives without their licenses (curl, OpenSSL, zlib, zstd and libpsl).
