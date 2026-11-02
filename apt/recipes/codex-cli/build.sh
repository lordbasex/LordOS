# apt/recipes/codex-cli/build.sh — OpenAI's Codex CLI (the static musl binary of the upstream release,
# its SHA-256 pinned and its sigstore bundle verified by apt/build.sh) as the .deb "codex-cli".
. /recipes/lib.sh

archive=/in/$(cat /in/ARCHIVE)
case "$ARCH" in amd64) triple=x86_64-unknown-linux-musl ;; arm64) triple=aarch64-unknown-linux-musl ;; *) die "arch $ARCH" ;; esac
w=$(mktemp -d); r=$w/root
mkdir -p "$r/usr/bin" "$r/usr/share/doc/$PKG" "$r/DEBIAN"
tar -xzf "$archive" -C "$w" "codex-$triple"
case "$(file -b "$w/codex-$triple")" in ELF*statically\ linked*|ELF*static-pie*) ;; *) die "codex-$triple is not a static ELF: $(file -b "$w/codex-$triple")" ;; esac
install -m 0755 "$w/codex-$triple" "$r/usr/bin/codex"

copyright_file "$r/usr/share/doc/$PKG/copyright" codex https://github.com/openai/codex Apache-2.0 /in/LICENSE
install -m 0644 /in/NOTICE "$r/usr/share/doc/$PKG/NOTICE"

cat > "$r/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: $MAINTAINER
Installed-Size: $(installed_size "$r")
Recommends: bubblewrap, ripgrep, git, ca-certificates
Section: devel
Priority: optional
Homepage: https://github.com/openai/codex
Description: OpenAI's coding agent for the terminal (Codex CLI)
 Codex is a coding agent from OpenAI that runs locally in the terminal: it
 reads, edits and runs code in the current folder, asking first according to
 its approval mode. Sign in with a ChatGPT account or an API key.
 .
 This is the upstream static (musl) release binary, verified against its pinned
 SHA-256 and its sigstore signature, packaged for LordOS. On Linux Codex uses
 bubblewrap to sandbox the commands it runs.
EOF
build_deb "$r"
