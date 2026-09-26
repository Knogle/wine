# Experimental Wine 11.3 hotfix for MTA:SA

This branch contains two Wine compatibility fixes for Multi Theft Auto: San
Andreas. A native 32-bit prototype successfully started unmodified MTA build
24149 and connected to a server in a user-operated test on 2026-09-26.

**Experimental, native Win32 only. This is not an official Wine, MTA, or Proton
release. WOW64 and Proton are not supported by this hotfix.** The result is not
a guarantee that other builds, servers, integrity checks, or longer sessions
will work.

## Source and scope

- Base: Wine 11.3, commit
  `222c976140d1b66c71769296f856f6523782b6c9`.
- Branch: `mta-hotfix/wine-11.3` in `knogle/wine`.
- Related application report:
  [MTA issue 4783](https://github.com/multitheftauto/mtasa-blue/issues/4783).
- Tested MTA build: 24149; the unchanged `netc.dll` SHA-256 was
  `ae0058d2c10d0cec6573f5bc4024d8ea32fee27f9784de347a0ecebf21f2eef9`.

The functional changes are separate commits:

1. **Crypt32:** support `CERT_SHA256_HASH_PROP_ID` (107), including computation
   of an uncached certificate SHA-256 hash and storage of the property. This
   addresses a certificate-property lookup failure during startup.
2. **NT file paths:** create Wine's real `\SystemRoot` object-manager symbolic
   link and resolve it for ordinary native file lookups. Applications can then
   open the actual `\SystemRoot\System32\ntdll.dll` through the existing file
   implementation instead of receiving `STATUS_OBJECT_PATH_NOT_FOUND`.

The second failure prevented MTA's backend initialization and led to a
misleading dispatcher-integrity error. This fix makes the real file available;
it does not fabricate handles, image bytes, or successful integrity verdicts.
MTA binaries are not patched, its checks are not disabled, and Wine detection
is not hidden. Diagnostic hooks and debugger scripts are not part of the
hotfix.

## Build a separate runner

Use an x86 Linux build environment with Wine's development dependencies,
including **32-bit** development/runtime libraries and i686 MinGW C/C++
compilers. The supplied recipe requires X11, OpenGL, Vulkan, FreeType,
Fontconfig, and PulseAudio development support; it deliberately fails when
these requested components are unavailable. See the upstream [requirements and compilation instructions](README.md).
Review configure's dependency checks, especially graphics, audio, and networking;
a build without the relevant components is not a usable game runner.

```sh
git clone --single-branch --branch mta-hotfix/wine-11.3 \
  https://github.com/knogle/wine.git wine-mta-hotfix
cd wine-mta-hotfix
git rev-parse HEAD

# Supply a new absolute output directory, outside the source tree.
./tools/mta-hotfix/build-runner.sh /absolute/path/to/mta-hotfix-build 8
```

The helper requires a clean committed checkout and builds and installs into
the output directory; it does not install
over system Wine, download games, or start a game. Keep the printed source
commit with any build you distribute. The expected outputs include a `runner/`
directory, `wine-mta-hotfix-win32.tar.xz`, the matching
`wine-mta-hotfix-source.tar.xz`, and `SHA256SUMS`. Build metadata is included in
`runner/share/mta-hotfix/BUILD-INFO.txt`; distribute both archives together.
This is a source-build workflow, not a link to an already tested binary release.

The initially successful experiment used rebuilt core components in an
existing Wine 11.3 runner, retaining its graphics/audio modules. A later complete
source build was packaged and checked separately; see the [full-build test
record](tools/mta-hotfix/FULL-BUILD-TESTS.md). This does **not** mean that every
resulting build has passed the original gameplay test. Runtime dependencies
still depend on the distribution and toolchain used; the archive is not a
self-contained universal Linux runtime. The packaging helper retains component
notices and font/SVG attribution alongside Wine's root license files.

## Try it without changing your normal prefix

Choose a new, dedicated absolute prefix path. Do not point this experimental
runner at your normal Wine, Lutris, or Steam prefix. The launcher requires an
explicit `WINEPREFIX` and rejects an existing non-Win32 prefix.

```sh
WINEPREFIX=/absolute/path/to/new-mta-test-prefix WINEARCH=win32 \
  /absolute/path/to/mta-hotfix-build/runner/bin/wine-mta wineboot -u

WINEPREFIX=/absolute/path/to/new-mta-test-prefix \
  /absolute/path/to/mta-hotfix-build/runner/bin/wine-mta \
  /absolute/path/to/your-installer.exe
```

Install your own legitimate GTA: San Andreas copy and MTA into that prefix,
then use the same launcher for MTA. No game files or MTA binaries are included
in this repository or runner. Do not reuse a prefix initialized with a
different runner: the on-disk `ntdll.dll` must match the runner's DLL. In the
initial experiment, `wineboot -u` alone did not refresh a previously installed
Ntdll; starting with a fresh prefix avoids that particular stale-file problem.

The launcher has no debugger, automatic server connection, or test timeout.
Stopping it and returning to your usual launcher leaves the separately chosen
normal prefix unchanged. A Wine prefix is an organizational boundary, not a
security sandbox; run only software you trust.

## What was checked

The certificate-property test run reported 639 tests, zero failures, and zero
skips. Targeted read-only native API probes compared the original Wine build,
the patched native 32-bit build, and native 64-bit Windows:

| Probe | Original Wine | Patched native Win32 |
| --- | --- | --- |
| Open `\SystemRoot\System32\ntdll.dll` | `C000003A`, no handle | Success, real readable PE file |
| Compare alias with `\??\C:\windows\system32\ntdll.dll` | Alias unavailable | Same volume/file identity and PE32 architecture |
| Missing final file | Path lookup failed early | `C0000034` (`STATUS_OBJECT_NAME_NOT_FOUND`) |
| Missing intermediate directory | `C000003A` | `C000003A` (`STATUS_OBJECT_PATH_NOT_FOUND`) |
| Lookalikes `SystemRootX` and `SystemRoot32` | Rejected | Still rejected |
| Length-bounded name without a NUL terminator | Alias unavailable | Successfully resolved |

Across 63 directly comparable API-status cases, 40 were unchanged and 21
matched the Windows reference after the fix. Two more reached the real missing
SysWOW64 directory in the pure Win32 prefix. No previously Windows-matching
status became worse in this set. This is a targeted regression result, not a
general Windows-conformance claim: the reference architectures differed, and
all executed probes reported that they were not running under WOW64.

The application test confirmed successful backend initialization and subsequent
API dispatch with unchanged MTA files. A later run without a debugger ended
after approximately 90 seconds, before its 180-second timeout. The human tester
explicitly confirmed a successful server connection and that they closed MTA
themselves. **This was not a completed three-minute endurance test.**

### Run the standalone probes

The sources in [tools/mta-hotfix/probes](tools/mta-hotfix/probes) use read-only
native file operations and do not require MTA. Build them with an i686 MinGW
compiler, then run them using the same dedicated prefix and runner:

```sh
i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -Werror -O2 \
  tools/mta-hotfix/probes/systemroot-open-probe.c -o /tmp/systemroot-open-probe.exe
i686-w64-mingw32-gcc -std=c99 -Wall -Wextra -Werror -O2 \
  tools/mta-hotfix/probes/systemroot-regression-probe.c -o /tmp/systemroot-regression-probe.exe

WINEPREFIX=/absolute/path/to/new-mta-test-prefix \
  /absolute/path/to/mta-hotfix-build/runner/bin/wine-mta /tmp/systemroot-open-probe.exe
WINEPREFIX=/absolute/path/to/new-mta-test-prefix \
  /absolute/path/to/mta-hotfix-build/runner/bin/wine-mta /tmp/systemroot-regression-probe.exe
```

For the simple probe, the System32 alias and DOS control should both report
`status=00000000`, `read_ok=1`, and `magic=4d5a`. Failure of SysWOW64 lookups is
expected in a pure Win32 prefix where that directory is absent. In the JSONL
regression output, the System32 comparison should report `same_file=1` and
`same_pe_arch=1`, with `machine=014c` and `optional_magic=010b` on successful
image opens. Known incomplete cases below are reported, not hidden. Exit code
zero means the probe finished collecting observations, **not** that all cases
passed or matched Windows. Compare statuses and file identity explicitly.

For a native 64-bit Windows reference, compile with `x86_64-w64-mingw32-gcc`
instead, and run the resulting executable only in an environment that permits
it. Such a run does not validate 32-bit-on-64-bit redirection. Do not disable or
bypass host application-control policies to run a probe.

## Known limitations

- The new resolver explicitly excludes WOW64. Exact WOW64 file-redirection
  behavior is still an acceptance-test gate. An attempted 32-bit Windows
  contract probe was blocked by the machine's Device Guard policy; that policy
  was not bypassed. Native 64-bit Windows results do not answer the WOW64 case.
- Without `OBJ_CASE_INSENSITIVE`, lowercase/uppercase alias handling differs
  from the observed Windows behavior.
- `OBJ_DONT_REPARSE` and `OBJ_OPENLINK` behavior remains incomplete. These flags
  are excluded from the new helper. `FILE_OPEN_REPARSE_POINT` worked in the
  tested alias case.
- Relative file opens from a real file-directory handle worked. Relative
  attribute queries still fail, as they already did for the DOS control path.
  Relative paths under an NT object-directory handle are not implemented here.
- Opening the symbolic link requests `SYMBOLIC_LINK_QUERY`. Its behavior under
  restricted or impersonated tokens still needs comparison with ordinary link
  traversal. Do not weaken permissions or add silent fallback paths to mask
  that open question.
- The separate initialization problem observed in MTA build 24124 is not fixed
  by these changes. No general multiplayer, anti-cheat, or long-session
  compatibility is claimed.

## Reporting and redistribution

For a useful report, include the branch commit, build configuration,
distribution, prefix architecture, exact MTA build, minimal reproduction, and
expected versus observed result. Remove credentials, personal paths, server
addresses, and unrelated personal data from logs before sharing them. Do not
upload proprietary DLLs, game files, or process dumps.

Keep Wine's [LICENSE](LICENSE), [COPYING.LIB](COPYING.LIB), copyright notices,
and applicable component licenses when redistributing. Preserve corresponding
source, the exact commit and build instructions alongside any binary package;
the existing license terms remain applicable. Do not label an untested package
as an official or fully compatible Proton release.

Investigation, implementation, tests, and publication materials were prepared
with AI coding-agent assistance. A human user performed and confirmed the
successful in-game connection and normal exit. Follow-up commits should record
the request/goal, measured cause, implementation reasoning, actual tests, and
remaining limitations rather than just saying "fix MTA".
