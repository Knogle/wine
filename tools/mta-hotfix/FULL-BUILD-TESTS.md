# Complete native Win32 build: 2026-09-26

This records a local, complete source build, distinct from the earlier hybrid
prototype. It is not an official Wine/MTA/Proton release or a claim that all
multiplayer servers work.

## Exact code and artifacts

Compiled source commit: `9dbe8f8a36d4a2d78d020e437d910e44ca3df087`.
The source archive's embedded Git ID and the runner's BUILD-INFO agree.
The loader reports `wine-11.3-3-g9dbe8f8`.

```text
c74a90e4606c8eba6c367ca0c0b95a9eb7fc658022e2e0bbc33d883b6816f299  wine-mta-hotfix-source.tar.xz
3160ea05bd49af54b4ed4a0c528f9d473f1ddafce05ac0c2c2057c507cfba94d  wine-mta-hotfix-win32.tar.xz
```

The binary archive is approximately 121 MiB and the source archive 33 MiB.
These hashes identify the archives attached to the [experimental Win32
preview release](https://github.com/Knogle/wine/releases/tag/wine-11.3-mta-hotfix-win32-r1).
The release tag points to the compiled commit above, not the later
branch commits that add packaging automation and this report.

All installed Wine components came from this build. No DLLs from the hybrid
runner, Microsoft runtimes, MTA files, GTA files, or prefixes were packaged.
As a post-build packaging step, 71 unchanged component-notice/font/SVG files
were extracted from the same source commit, and an IJG acknowledgement was
added. The binary archive was then regenerated. No executable code changed.
The subsequent helper change automates this notice-preservation step for
future builds; the artifact source commit above remains unchanged.

## Build environment and limits

- Dedicated Fedora 43 Toolbox, native GCC 15.3.1 and i686 MinGW GCC 15.2.1,
  24 jobs. These distinct versions are recorded in the packaged BUILD-INFO;
  an earlier report incorrectly described both compilers as 15.2.1.
- Recipe: `tools/mta-hotfix/build-runner.sh /absolute/new/output 24`.
- `PKG_CONFIG_LIBDIR=/usr/lib/pkgconfig:/usr/share/pkgconfig`, with inherited
  `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` unset.
- Enabled: native Win32, X11/Wayland, OpenGL/Vulkan, FreeType/Fontconfig,
  PulseAudio/ALSA, GStreamer, GnuTLS, SDL, USB/Udev, D-Bus.
- Optional support absent: FFmpeg, smartcards, digital cameras, OSSv4, ISDN,
  Kerberos, Samba NetAPI. GStreamer provides the tested media path.

The dedicated Toolbox avoids Fedora's conflicting same-path i686/x86_64 GIR
files. Do not force-overwrite GLib/GStreamer development files in an existing
mixed development environment. Explicitly choose i686 development packages,
including their transitive pkg-config dependencies; a successful
`pkg-config --modversion` alone does not prove `--cflags` works. In this build,
excluding `glib2-devel.x86_64` and `gstreamer1-devel.x86_64` from the fresh
container's package transaction avoided the conflicting candidates.

Runtime tests used Fedora 44. The highest directly requested glibc symbol
version in the installed ELF files is `GLIBC_2.38`; this is a necessary lower
bound, not proof of portability to arbitrary older distributions. Host
libraries are not bundled. The test host resolves the core graphics, audio,
TLS and media dependencies. Its optional 32-bit `libOpenCL.so.1` and
`libsane.so.1` are absent, so OpenCL/scanner support is not validated there.

## Executed checks

The final binary archive was extracted to a different path containing spaces.
A fresh native Win32 prefix was created using that installed runner, with
inherited `WINEDLLPATH`, `LD_LIBRARY_PATH`, `WINELOADER` and `WINESERVER` unset.
Prefix Ntdll and Crypt32 match the corresponding installed PE files byte for
byte. A trace of the independent file probe confirmed the relocated loader,
wineserver and Ntdll paths. All 16 installed symlinks are relative and none
refer to the source/build tree.

| Check | Observed result |
| --- | --- |
| `wineboot -u` in a fresh prefix | Exit 0 |
| Crypt32 `cert` | 639 tests, 0 failures, 0 skipped |
| SystemRoot file probe | Real readable Ntdll through alias and DOS control |
| SystemRoot regression probe | All 66 observed API statuses match the successful prototype; alias/control identify the same file and PE32 architecture |
| Graphics/audio/Winsock smoke | D3D9 HAL device, Clear/Present, output-device enumeration and unconnected socket succeeded |
| Quartz `waveparser` | 320 tests, 13 existing todo markers, 0 failures, 0 skipped |
| Quartz `mpeglayer3` | 417 tests, 1 existing todo marker, 0 failures, 0 skipped |

The media tests used Wine's embedded test assets, not downloaded codecs or
proprietary media. Audio enumeration did not play or record sound. The socket
smoke did not connect to a server. A HAL-device result alone is not proof of
a particular physical GPU or game performance.

The targeted test executables are built by this recipe. Run them with the
installed runner and a dedicated prefix, not via a build-tree Wine selected
by `make check`:

```sh
WINEPREFIX=/absolute/test-prefix /absolute/runner/bin/wine-mta \
  /absolute/build/dlls/crypt32/tests/i386-windows/crypt32_test.exe cert
WINEPREFIX=/absolute/test-prefix /absolute/runner/bin/wine-mta \
  /absolute/build/dlls/quartz/tests/i386-windows/quartz_test.exe waveparser
WINEPREFIX=/absolute/test-prefix /absolute/runner/bin/wine-mta \
  /absolute/build/dlls/quartz/tests/i386-windows/quartz_test.exe mpeglayer3
```

The standalone graphics/audio/socket probe is provided as
[probes/full-runner-smoke.c](probes/full-runner-smoke.c):

```sh
i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -Werror -O2 \
  tools/mta-hotfix/probes/full-runner-smoke.c -o /tmp/full-runner-smoke.exe \
  -ld3d9 -ldsound -lwinmm -lws2_32 -luser32
WINEPREFIX=/absolute/test-prefix /absolute/runner/bin/wine-mta \
  /tmp/full-runner-smoke.exe
```

Require the final JSON summary to report `failures=0`, not merely a completed
process. The probe opens a small test window and then closes it.

## Application check

Unmodified MTA 24149 reached its rendered main menu with this complete runner;
the menu was visually inspected. No integrity-error dialog was observed in
that startup. A new server-connection or endurance test for this particular
binary was not established by that observation. The earlier human-confirmed
server connection belongs to the hybrid prototype and must not be silently
attributed to this full build.

Only the existing legitimate game/application directories were copied into
the fresh test prefix; no old Wine registry or Windows DLLs were copied.
The first attempt found a missing normal MTA Common data directory (CL03).
Creating the installer-required directory resolved that setup error. GTA's
ordinary first-start graphics-device dialog was then confirmed through its
OK button. Neither action disables any integrity check or changes game code.
Other users should install their own GTA/MTA normally into a fresh prefix.

The unchanged MTA `netc.dll` hash was
`ae0058d2c10d0cec6573f5bc4024d8ea32fee27f9784de347a0ecebf21f2eef9`.
Native Win32 only: the existing WOW64, object-path, permission and
case-sensitivity limitations in [HOTFIX-MTA.md](../../HOTFIX-MTA.md) still
apply. The user's production prefix and system Wine were not replaced.
