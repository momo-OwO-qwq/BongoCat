#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo 'Debian packaging requires Linux.' >&2
  exit 1
fi
build_dir=$(cd "${1:?Usage: build-deb.sh BUILD_DIRECTORY}" && pwd)
source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
name=$(tr -d '\r\n' < "$build_dir/bongocat-package-name.txt")
[[ $name =~ ^(BongoCat-Diagnostic|BongoCat)-([0-9][A-Za-z0-9.+-]*)-linux-(x64|arm64)$ ]] || {
  echo "Unexpected package name: $name" >&2
  exit 1
}
version=${BASH_REMATCH[2]}
package_arch=${BASH_REMATCH[3]}
case $package_arch in
  x64) deb_arch=amd64 ;;
  arm64) deb_arch=arm64 ;;
esac
if [[ $name == BongoCat-Diagnostic-* ]]; then
  deb_package=bongocat-diagnostic
else
  deb_package=bongocat
fi
host_arch=$(dpkg --print-architecture)
if [[ $host_arch != "$deb_arch" ]]; then
  echo "Debian packaging must run on $deb_arch, found $host_arch." >&2
  exit 1
fi
command -v dpkg-shlibdeps >/dev/null ||
  { echo 'dpkg-shlibdeps is required to compute dependencies (dpkg-dev).' >&2; exit 1; }
debian_maintainer='vladelaina <vladelaina@users.noreply.github.com>'

work=$(mktemp -d "$build_dir/deb.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
stage="$work/stage"
mkdir -p "$build_dir/dist" "$stage/usr/lib/bongocat" "$stage/usr/bin" \
  "$stage/usr/share/applications" "$stage/usr/share/pixmaps" "$stage/DEBIAN"

# Runtime component installs the executable and its assets side by side,
# which is what SDL_GetBasePath() expects at startup.
cmake --install "$build_dir" --component Runtime --prefix "$stage/usr/lib/bongocat"
ln -s ../lib/bongocat/BongoCat "$stage/usr/bin/BongoCat"
install -m 0644 "$source_dir/packaging/linux/bongocat.desktop" \
  "$stage/usr/share/applications/bongocat.desktop"
install -m 0644 "$source_dir/resources/assets/bongocat.png" \
  "$stage/usr/share/pixmaps/bongocat.png"

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
for required in "${required_files[@]}"; do
  test -s "$stage/$required" || {
    echo "Missing Debian package resource: $required" >&2
    exit 1
  }
done

# Derive runtime dependencies from the packaged executable's dynamic links.
# dpkg-shlibdeps insists on finding a debian/control source stanza first.
mkdir -p "$work/debian"
cat > "$work/debian/control" <<EOF
Source: $deb_package
Section: utils
Priority: optional
Maintainer: $debian_maintainer
Standards-Version: 4.6.2
Homepage: https://github.com/vladelaina/BongoCat

Package: $deb_package
Architecture: any
Depends: \${shlibs:Depends}
Description: An animated desktop cat that reacts to your input
EOF
depends=$(cd "$work" &&
  dpkg-shlibdeps -O -e"$stage/usr/lib/bongocat/BongoCat" 2> "$work/shlibdeps.log" |
  sed -n 's/^shlibs:Depends=//p') || {
  cat "$work/shlibdeps.log" >&2
  echo 'dpkg-shlibdeps failed; cannot compute Debian dependencies.' >&2
  exit 1
}
if [[ -z $depends ]]; then
  cat "$work/shlibdeps.log" >&2
  echo 'dpkg-shlibdeps produced no dependency information.' >&2
  exit 1
fi

cat > "$stage/DEBIAN/control" <<EOF
Package: $deb_package
Version: $version
Section: utils
Priority: optional
Architecture: $deb_arch
Maintainer: $debian_maintainer
Homepage: https://github.com/vladelaina/BongoCat
Depends: $depends
Description: An animated desktop cat that reacts to your input
 BongoCat renders an animated desktop character that reacts to keyboard,
 mouse and application input in real time.
 .
 The runtime is installed under /usr/lib/bongocat with a launcher at
 /usr/bin/BongoCat and a desktop entry in the application menu.
EOF

output="$build_dir/dist/$name.deb"
dpkg-deb -Zxz --root-owner-group --build "$stage" "$output" > /dev/null
test -s "$output"
(cd "$build_dir/dist" && sha256sum "$name.deb" > "$name.deb.sha256")
echo "Debian package ready: $output"
