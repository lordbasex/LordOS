#!/usr/bin/env bash
# Publishes a release's signed list on lordos.org, where Machines' LordOS card reads it first
# (assets/machines/systems.json, "list"; docs/VIRTUALIZATION.md 3.9, "The LordOS card"): GitHub's
# release downloads sometimes answer 403 on their signed redirect, so the list (SHA256SUMS) and its
# signature (SHA256SUMS.sig) are also served from releases/<tag>/ on the site. The ISOs themselves
# still come from GitHub.
#
#   tools/release/publish-lists.sh <tag> <dir-with-SHA256SUMS-and-SHA256SUMS.sig>
#
# The list is uploaded as SHA256SUMS.txt (CloudFront serves extensionless paths as pretty-URL pages),
# same bytes. The signature is checked with tools/release/lordos-release.pub.pem before anything is
# uploaded; Machines checks it again with the key compiled into LordOS, so this site is not trusted
# for anything. Both are text/plain and revalidated on every visit. tools/web/deploy.sh never deletes
# releases/. The profile, bucket and distribution come from tools/web/aws-env.sh, like deploy.sh.
set -euo pipefail

tag=${1:?usage: publish-lists.sh <tag> <dir-with-SHA256SUMS-and-SHA256SUMS.sig>}
dir=${2:?usage: publish-lists.sh <tag> <dir-with-SHA256SUMS-and-SHA256SUMS.sig>}
case "$tag" in v[0-9]*) ;; *) echo "not a release tag: $tag (v<version>[-suffix])" >&2; exit 1 ;; esac
case "$tag" in *[!A-Za-z0-9.+-]*) echo "unexpected characters in the tag: $tag" >&2; exit 1 ;; esac

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
pub=$ROOT/tools/release/lordos-release.pub.pem
sums=$dir/SHA256SUMS
sig=$dir/SHA256SUMS.sig
[ -f "$sums" ] && [ -f "$sig" ] || { echo "no SHA256SUMS and SHA256SUMS.sig in $dir" >&2; exit 1; }
command -v openssl >/dev/null || { echo "openssl is needed" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
base64 -d < "$sig" > "$tmp/sig.bin" 2>/dev/null || base64 -D < "$sig" > "$tmp/sig.bin"
openssl pkeyutl -verify -pubin -inkey "$pub" -rawin -in "$sums" -sigfile "$tmp/sig.bin" >/dev/null ||
    { echo "SHA256SUMS.sig does not verify with $pub: not uploading" >&2; exit 1; }

. "$ROOT/tools/web/aws-env.sh"
lordos_aws_env LORDOS_AWS_PROFILE LORDOS_S3_BUCKET LORDOS_CF_DISTRIBUTION
aws() { command aws --profile "$LORDOS_AWS_PROFILE" "$@"; }

dest="s3://$LORDOS_S3_BUCKET/releases/$tag"
aws s3 cp "$sums" "$dest/SHA256SUMS.txt" --cache-control "no-cache" --content-type "text/plain; charset=utf-8" --only-show-errors
aws s3 cp "$sig" "$dest/SHA256SUMS.sig" --cache-control "no-cache" --content-type "text/plain; charset=utf-8" --only-show-errors
id=$(aws cloudfront create-invalidation --distribution-id "$LORDOS_CF_DISTRIBUTION" --paths "/releases/*" --query Invalidation.Id --output text)

# what the site now serves: the same bytes, and a signature that still verifies
if command -v curl >/dev/null; then
    sleep 5
    curl -fsS "https://lordos.org/releases/$tag/SHA256SUMS.txt" -o "$tmp/served.txt"
    curl -fsS "https://lordos.org/releases/$tag/SHA256SUMS.sig" -o "$tmp/served.sig"
    if cmp -s "$sums" "$tmp/served.txt" && cmp -s "$sig" "$tmp/served.sig"; then
        echo "lordos.org serves releases/$tag/SHA256SUMS.txt and SHA256SUMS.sig (invalidation $id)"
    else
        echo "uploaded (invalidation $id), but lordos.org still serves other bytes: check again once the invalidation is done" >&2
    fi
else
    echo "uploaded releases/$tag/SHA256SUMS.txt and SHA256SUMS.sig (invalidation $id)"
fi
