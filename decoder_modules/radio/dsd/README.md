# DSD and oldDSD for SDR++ JP

The Radio menu exposes two digital voice modes: **DSD** and **oldDSD**. They
are part of JP's existing `radio` plugin, so there is no separate VHF plugin
to enable. All other JP modules remain in their original locations.

The demodulator, user interface, slicer, and protocol processing sources in
`src/` were ported from `decoder_modules/ch_extravhf_decoder` in
[SDR++ Brown](https://github.com/sannysanoff/SDRPlusPlusBrown), archive
commit `ae7e32b0d7ee11bbc907ca7925c9b16dd22c8083`. That module credits
Cropinghigh's [VHF voice plugin](https://github.com/cropinghigh/sdrpp-vhfvoiceradio).
The original GPL-3.0 license is kept in `LICENSE`. The DSD protocol code
also contains its original ISC notices.

The voice codec under `mbelib/` is copied from
[szechyjs/mbelib](https://github.com/szechyjs/mbelib), commit
`9a04ed5c78176a9965f3d43f7aa1b1f5330e771f`, and retains its ISC notice
in `mbelib/COPYRIGHT`. These codec files are built as a private static library
for the Radio plugin. ITPP is supplied by the system (`libitpp-dev` at build
time and its runtime library when installed).

## Linux packages and AppImage

`Build Binaries` runs on pull requests, version tags, and manual dispatch. Its Linux
matrix builds DSD-enabled `.deb` packages for Debian bullseye, bookworm,
trixie, and sid and Ubuntu focal, jammy, noble, and resolute on amd64 and
arm64. It also builds DSD-enabled x86_64 and aarch64 AppImages with ITPP
bundled. The separate `Build SCI Linux packages` workflow builds the five
amd64 `.deb` packages for Debian bookworm/trixie and Ubuntu jammy/noble/resolute,
plus the x86_64 AppImage. It runs on master pushes, publishes successful builds
to the nightly release, and can also be started manually. On a local Noble
system with the normal JP build dependencies and `libitpp-dev` installed:

```sh
cmake --preset ci-linux-deb -DOPT_BUILD_DSD=ON
cmake --build build --parallel 4
(cd build && cpack)
```

The resulting `build/sdrpp_debian_pkg.deb` contains the JP application and
the Radio plugin with both DSD modes. The option `OPT_BUILD_DSD=OFF` leaves
the original radio plugin without these modes. The option defaults to OFF;
the Linux packaging workflows explicitly turn it on and install `libitpp-dev`.
The GitHub Actions workflows package Linux only. JP's source files for other
platforms remain in the repository; no Windows, macOS, or Android CI jobs run.

## Updating from the JP parent repository

`Sync JP upstream` checks the original JP repository every Monday, and can
also be run manually in the Actions tab. When JP has new commits, it merges
them into a branch, opens a pull request in this fork, and starts the Linux
package/AppImage build. Review the PR and its build before merging into master.
If JP edits the same lines as SCI, the sync run reports a merge conflict for
manual resolution. GitHub may require enabling Actions to create pull requests
under repository Settings > Actions > General > Workflow permissions.

On each push to master, `Build SCI Linux packages` refreshes the nightly
release after its five `.deb` builds and AppImage succeed.

For a tagged release, update `VERSION_STR` in `core/src/version.h`, commit and
push, then create and push a matching `v<version>` tag. The workflow checks
that the tag matches the header. For example, `1.7.0-beta` requires
`v1.7.0-beta`; bump the version for later releases so package upgrades have a
newer version. GitHub Releases host the downloads; automatic upgrades through
`apt` require a separate signed APT repository.
