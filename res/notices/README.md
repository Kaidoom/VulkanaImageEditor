# Offline notices

`components.json.in` describes the source build's direct system dependencies.
CMake fills in the discovered versions and embeds the unchanged license copies
in `build/<configuration>/notices/`. Both the viewer and plain-text download use
that generated data. No network connection or private release files are needed.

The LGPL, GPL, Apache, GCC exception and WebP copyright copies were retained from
the reviewed upstream/distro notices. The minizip license is from minizip-ng
4.1.0; WebP's copyright and patent grant are from libwebp 1.6.0. The zlib license
copy is from 1.3.2; the build may use the compatible zlib-ng implementation.
Upstream references are in [THIRD_PARTY.md](../../THIRD_PARTY.md).

This is not an inventory of a bundled binary distribution. A distributor can
provide reviewed `notices.json` and `THIRD-PARTY-NOTICES.txt` through the
`VULKANA_NOTICES_DIRECTORY` CMake option. Their version/channel must match the
build. An optional `VULKANA_DISTRIBUTION_CMAKE` extension can add distribution
rules without making them dependencies of the source checkout.
