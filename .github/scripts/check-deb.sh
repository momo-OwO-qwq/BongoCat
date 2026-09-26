#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo 'Usage: check-deb.sh DEB [linux-x64|linux-arm64] [--skip-smoke]' >&2
  exit 2
}

deb=''
platform=''
skip_smoke=0
for arg in "$@"; do
  case $arg in
    --skip-smoke) skip_smoke=1 ;;
    linux-x64|linux-arm64) [[ -z $platform ]] || usage; platform=$arg ;;
    *) [[ -z $deb && -f $arg ]] || usage; deb=$(realpath "$arg") ;;
  esac
done
[[ -n $deb ]] || usage

test_runner=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test-unix.sh
name=$(basename "$deb" .deb)
[[ $name =~ ^(BongoCat-Diagnostic|BongoCat)-([0-9][A-Za-z0-9.+-]*)-linux-(x64|arm64)$ ]] || {
  echo "Unexpected package file name: $name" >&2
  exit 1
}
version=${BASH_REMATCH[2]}
package_arch=${BASH_REMATCH[3]}
case $package_arch in
  x64) deb_arch=amd64 ;;
  arm64) deb_arch=arm64 ;;
esac
if [[ -n $platform && $platform != "linux-$package_arch" ]]; then
  echo "Package file name says linux-$package_arch, expected $platform." >&2
  exit 1
fi
if [[ $name == BongoCat-Diagnostic-* ]]; then
  deb_package=bongocat-diagnostic
else
  deb_package=bongocat
fi

[[ $(dpkg-deb -f "$deb" Package) == "$deb_package" ]] ||
  { echo 'Unexpected Debian package name.' >&2; exit 1; }
[[ $(dpkg-deb -f "$deb" Version) == "$version" ]] ||
  { echo 'Debian package version does not match the file name.' >&2; exit 1; }
[[ $(dpkg-deb -f "$deb" Architecture) == "$deb_arch" ]] ||
  { echo "Debian package architecture does not match $deb_arch." >&2; exit 1; }
depends=$(dpkg-deb -f "$deb" Depends)
[[ -n $depends ]] || { echo 'Debian package declares no dependencies.' >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
root="$work/root"
dpkg-deb -x "$deb" "$root"
required_files=(
  usr/lib/bongocat/BongoCat
  usr/lib/bongocat/assets/bongocat.png
  usr/lib/bongocat/assets/locales/en-US.json
  usr/lib/bongocat/assets/models/standard/cat.model3.json
  usr/lib/bongocat/assets/models/standard/demomodel.moc3
  usr/lib/bongocat/assets/models/standard/demomodel.1024/texture_00.png
  usr/share/applications/bongocat.desktop
  usr/share/pixmaps/bongocat.png
)
if [[ $deb_package == bongocat ]]; then
  # Cubism shaders ship only with the full Live2D runtime.
  required_files+=(
    usr/lib/bongocat/assets/FrameworkShaders/VertShaderSrc.vert
    usr/lib/bongocat/assets/FrameworkShaders/FragShaderSrc.frag
    usr/lib/bongocat/assets/FrameworkShaders/VertShaderSrcBlend.vert
    usr/lib/bongocat/assets/FrameworkShaders/FragShaderSrcBlend.frag
  )
fi
for file in "${required_files[@]}"; do
  test -s "$root/$file" || { echo "Missing Debian resource: $file" >&2; exit 1; }
done
test -L "$root/usr/bin/BongoCat" ||
  { echo 'Missing /usr/bin/BongoCat launcher symlink.' >&2; exit 1; }
if command -v desktop-file-validate > /dev/null; then
  desktop-file-validate "$root/usr/share/applications/bongocat.desktop"
fi
echo "Debian package layout verified: $deb_package $deb_arch $version"

if [[ $skip_smoke == 1 ]]; then
  echo 'Smoke test skipped.'
  exit 0
fi

# Launch through the packaged symlink to prove asset lookup keeps working.
bash "$test_runner" env BONGO_CAT_DISABLE_NEARBY_MODEL_SCAN=1 \
  "$root/usr/bin/BongoCat" --ci-smoke --ci-ignore-global-input \
  --ci-live2d-scenario=visual-consistency "--storage-root=$work/smoke-data"
mapfile -d '' audits < <(find "$work/smoke-data" -name live2d-audit.txt -print0)
[[ ${#audits[@]} == 1 ]] || { echo 'Live2D audit report missing.' >&2; exit 1; }
if [[ $deb_package == bongocat-diagnostic ]]; then
  grep -q 'renderer=diagnostic' "${audits[0]}" ||
    { echo 'Diagnostic renderer not reported.' >&2; exit 1; }
else
  grep -qx 'renderer=cubism-native' "${audits[0]}" ||
    { echo 'Cubism renderer not reported.' >&2; exit 1; }
  grep -qx 'assertions=passed' "${audits[0]}" ||
    { echo 'Live2D smoke assertions failed.' >&2; exit 1; }
fi
echo 'Debian package layout and Live2D smoke test passed.'
