#!/bin/sh
# headers.sh <cbindgen> <include/rice> — librice's two C headers written by cbindgen 0.29 (debian/rules), each with the
# version defines (RICE_PROTO_MAJOR…, RICE_IO_MAJOR…) that cargo-c put in its own, empty, header. Fails when a header
# declares no function.
set -eu
CBG=$1 DIR=$2
mkdir -p debian/headers
for c in rice-proto rice-io; do
    h=$DIR/$c.h
    grep -E '^#define RICE_[A-Z]+_(MAJOR|MINOR|PATCH) ' "$h" > "debian/headers/$c.defines"
    [ "$(wc -l < "debian/headers/$c.defines")" = 3 ] || { echo "headers.sh: no version defines in cargo-c's $h" >&2; exit 1; }
    "$CBG" --config "$c/cbindgen.toml" --crate "$c" --output "debian/headers/$c.h" "$(pwd)"
    guard=$(sed -n 's/^#define \(LIBRICE_[A-Z]*_H\)$/\1/p' "debian/headers/$c.h")
    [ -n "$guard" ] || { echo "headers.sh: no include guard in $c.h" >&2; exit 1; }
    sed "/^#define $guard\$/r debian/headers/$c.defines" "debian/headers/$c.h" > "$h"
    grep -q 'rice_[a-z_]*(' "$h" || { echo "headers.sh: $h declares no function" >&2; exit 1; }
    echo "headers.sh: $h, $(grep -c 'rice_[a-z_]*(' "$h") functions"
done
