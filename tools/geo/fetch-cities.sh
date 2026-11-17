#!/bin/sh
# fetch-cities.sh — refreshes assets/geo/cities.tsv, the offline list of cities the weather widget lets a person pick
# (docs/WORKSPACES.md, "Weather"). Run before each release:  tools/geo/fetch-cities.sh
#
# Source: GeoNames' cities15000 (every city of 15,000 people or more), CC BY 4.0 — https://www.geonames.org (credited
# in About). Downloaded into a new empty folder, read by reduce-cities.mjs from there (never run), and reduced to what
# LordOS needs: id, name, ASCII name, country, first-level region, latitude and longitude rounded to 2 decimals
# (~1 km), population. No alternate names, no other field.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
url=https://download.geonames.org/export/dump/cities15000.zip
echo "fetch-cities: $url" >&2
curl -fsSL --proto '=https' --tlsv1.2 --max-filesize 20000000 -o "$work/cities15000.zip" "$url"
( cd "$work" && unzip -q -o cities15000.zip cities15000.txt )
[ -s "$work/cities15000.txt" ] || { echo "fetch-cities: empty download" >&2; exit 1; }
mkdir -p "$root/assets/geo"
node "$here/reduce-cities.mjs" "$work/cities15000.txt" "$root/assets/geo/cities.tsv"
echo "fetch-cities: $(wc -l < "$root/assets/geo/cities.tsv") cities in assets/geo/cities.tsv" >&2
