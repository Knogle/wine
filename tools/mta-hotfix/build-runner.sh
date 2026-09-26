#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

die() { printf 'build-runner: %s\n' "$*" >&2; exit 1; }
[[ $# -ge 1 && $# -le 2 ]] || die "usage: $0 /absolute/new/output-directory [jobs]"
[[ $1 = /* ]] || die 'output directory must be absolute'
jobs=${2:-$(getconf _NPROCESSORS_ONLN)}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || die 'jobs must be a positive integer'
for tool in git make gcc g++ i686-w64-mingw32-gcc i686-w64-mingw32-g++ bison flex pkg-config tar xz sha256sum realpath; do
    command -v "$tool" >/dev/null || die "missing build tool: $tool"
done
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
output_dir=$(realpath -m -- "$1")
[[ ! -e $output_dir && ! -L $output_dir ]] || die 'output directory already exists; choose a new one'
case $output_dir/ in "$source_dir/"*) die 'output directory must be outside the source checkout' ;; esac
[[ $(uname -s) = Linux ]] || die 'this recipe targets Linux only'
case $(uname -m) in i?86|x86_64) ;; *) die 'this recipe targets x86 Linux only' ;; esac
# A clean commit lets each binary archive ship its exact corresponding sources.
[[ -z $(git -C "$source_dir" status --porcelain --untracked-files=all) ]] || die 'commit or remove local source changes first'
source_commit=$(git -C "$source_dir" rev-parse HEAD)
SOURCE_DATE_EPOCH=$(git -C "$source_dir" show -s --format=%ct HEAD)
export SOURCE_DATE_EPOCH
mkdir -- "$output_dir"
mkdir -- "$output_dir/build" "$output_dir/stage"
trap 'printf "Build stopped; diagnostic files remain in %s\n" "$output_dir" >&2' ERR

# A staged, relative-layout installation can move without a prefix-specific wrapper.
# Explicit graphics/audio requirements prevent silently publishing a headless build.
configure_args=(--prefix=/opt/wine-mta-hotfix --libdir=/opt/wine-mta-hotfix/lib
    --disable-win64 --with-x --with-opengl --with-vulkan --with-freetype
    --with-fontconfig --with-pulse)
cd -- "$output_dir/build"
"$source_dir/configure" "${configure_args[@]}" 2>&1 | tee "$output_dir/configure.log"
make -j "$jobs" 2>&1 | tee "$output_dir/build.log"
make install DESTDIR="$output_dir/stage" 2>&1 | tee "$output_dir/install.log"
mv -- "$output_dir/stage/opt/wine-mta-hotfix" "$output_dir/runner"
runner=$output_dir/runner
for required in bin/wine bin/wineserver lib/wine/i386-unix/ntdll.so \
    lib/wine/i386-unix/winex11.so lib/wine/i386-unix/winepulse.so \
    lib/wine/i386-windows/crypt32.dll lib/wine/i386-windows/ntdll.dll; do
    [[ -f $runner/$required ]] || die "incomplete runner: missing $required"
done
install -m 755 "$source_dir/tools/mta-hotfix/wine-mta" "$runner/bin/wine-mta"
mkdir -- "$runner/share/mta-hotfix"
cp -- "$source_dir/"{COPYING.LIB,LICENSE,LICENSE.OLD,AUTHORS,HOTFIX-MTA.md} "$runner/share/mta-hotfix/"
# Binary downloads need the bundled components' notices even when the matching
# source archive is downloaded separately. Keep asset metadata intact, and read
# every upstream notice from the recorded commit so the package stays traceable.
notice_dir=$runner/share/mta-hotfix/third-party
mkdir -- "$notice_dir"
git -C "$source_dir" ls-tree -r --name-only -z "$source_commit" > "$output_dir/notice-tree.z"
notice_paths=()
while IFS= read -r -d '' notice_path; do
    case $notice_path in
        fonts/*.sfd) notice_paths+=("$notice_path"); continue ;;
    esac
    notice_name=${notice_path##*/}
    case ${notice_name^^} in
        AUTHORS|COPYING*|COPYRIGHT*|LICENSE*|LICENCE*|NOTICE*|CREDITS*)
            notice_paths+=("$notice_path") ;;
    esac
done < "$output_dir/notice-tree.z"
notice_status=0
git -C "$source_dir" grep -z -l -i -e '<cc:license' "$source_commit" -- '*.svg' \
    > "$output_dir/notice-svg.z" || notice_status=$?
[[ $notice_status -le 1 ]] || die 'cannot read asset license metadata from source commit'
while IFS= read -r -d '' notice_match; do
    notice_paths+=("${notice_match#*:}")
done < "$output_dir/notice-svg.z"
[[ ${#notice_paths[@]} -gt 0 ]] || die 'source commit contains no license notices'
git -C "$source_dir" --literal-pathspecs archive --format=tar "$source_commit" \
    -- "${notice_paths[@]}" |
    tar --extract --file=- --directory="$notice_dir" --no-same-owner
install -m 644 "$source_dir/tools/mta-hotfix/ACKNOWLEDGEMENTS.txt" "$runner/share/mta-hotfix/"
{
    printf 'Source commit: %s\nArchitecture: traditional win32 only\n' "$source_commit"
    printf 'Configure:'; printf ' %q' "${configure_args[@]}"; printf '\n'
    gcc --version | sed -n '1p'
    i686-w64-mingw32-gcc --version | sed -n '1p'
    printf 'Host runtime libraries are not bundled. No runtime test was run by this script.\n'
} > "$runner/share/mta-hotfix/BUILD-INFO.txt"
# Source and binary artifacts belong together; neither contains the build workspace.
git -C "$source_dir" archive --format=tar --prefix=wine-source/ "$source_commit" |
    xz -T "$jobs" > "$output_dir/wine-mta-hotfix-source.tar.xz"
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
    -C "$output_dir" -cf - runner | xz -T "$jobs" > "$output_dir/wine-mta-hotfix-win32.tar.xz"
cd -- "$output_dir"
sha256sum wine-mta-hotfix-{source,win32}.tar.xz > SHA256SUMS
printf 'Created %s\nTest the runner before distributing both archives with SHA256SUMS.\n' "$output_dir"
