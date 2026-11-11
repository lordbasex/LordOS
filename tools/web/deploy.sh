#!/usr/bin/env bash
# Publishes the lordos.org static site (web/dist, built by `node web/build.mjs`) to the S3 bucket
# behind CloudFront. Hashed assets are cached for a year; pages, install.sh and the release key are
# revalidated on every visit.
#
#   node web/build.mjs && tools/web/deploy.sh
#
# The profile, bucket and distribution come from ~/.aws/.lordos-deploy-env (tools/web/aws-env.sh).
# The profile is a deploy user that can write to this bucket and invalidate this distribution, nothing
# else. Security headers (CSP, HSTS) live in the CloudFront response
# headers policy, not here.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIST="${1:-$ROOT/web/dist}"
. "$ROOT/tools/web/aws-env.sh"
lordos_aws_env LORDOS_AWS_PROFILE LORDOS_S3_BUCKET LORDOS_CF_DISTRIBUTION
PROFILE=$LORDOS_AWS_PROFILE
BUCKET=$LORDOS_S3_BUCKET
DISTRIBUTION=$LORDOS_CF_DISTRIBUTION

[ -f "$DIST/index.html" ] || { echo "no build in $DIST: run node web/build.mjs" >&2; exit 1; }
[ -f "$DIST/install.sh" ] || { echo "missing $DIST/install.sh" >&2; exit 1; }
git -C "$ROOT" show HEAD:tools/install/install.sh | cmp -s - "$DIST/install.sh" || { echo "dist/install.sh differs from the committed tools/install/install.sh" >&2; exit 1; }
[ -f "$DIST/lordos-release.pub.pem" ] || { echo "missing $DIST/lordos-release.pub.pem" >&2; exit 1; }
cmp -s "$ROOT/tools/release/lordos-release.pub.pem" "$DIST/lordos-release.pub.pem" || { echo "dist/lordos-release.pub.pem differs from tools/release/lordos-release.pub.pem" >&2; exit 1; }
if find "$DIST" -name '*.map' | grep -q .; then echo "source maps in $DIST" >&2; exit 1; fi

aws() { command aws --profile "$PROFILE" "$@"; }

# 1. Hashed assets first (their names change with their content), so no page points at a missing file.
aws s3 sync "$DIST/assets" "s3://$BUCKET/assets" --cache-control "public, max-age=31536000, immutable" --only-show-errors

# 2. Pages and the other root files, always revalidated.
aws s3 sync "$DIST" "s3://$BUCKET" --exclude "assets/*" --exclude "apt/*" --exclude "apt-staging/*" --exclude "keys/*" --exclude "install.sh" --exclude "lordos-release.pub.pem" --exclude ".well-known/*" \
  --cache-control "no-cache" --only-show-errors

# 3. install.sh (curl … | sh), the release key and security.txt as plain text, readable in a browser.
aws s3 cp "$DIST/install.sh" "s3://$BUCKET/install.sh" --cache-control "no-cache" --content-type "text/plain; charset=utf-8" --only-show-errors
[ -f "$DIST/.well-known/security.txt" ] && aws s3 cp "$DIST/.well-known/security.txt" "s3://$BUCKET/.well-known/security.txt" --cache-control "no-cache" --content-type "text/plain; charset=utf-8" --only-show-errors
aws s3 cp "$DIST/lordos-release.pub.pem" "s3://$BUCKET/lordos-release.pub.pem" --cache-control "no-cache" --content-type "text/plain; charset=utf-8" --only-show-errors

# 4. Remove pages and root files the build no longer produces. Old hashed assets stay for pages
#    that are already open in a browser; they are small and never referenced again. The APT
#    repository (apt/, apt-staging/) and its key (keys/) belong to tools/release/apt-repo.sh: never
#    touched here.
aws s3 sync "$DIST" "s3://$BUCKET" --delete --exclude "assets/*" --exclude "apt/*" --exclude "apt-staging/*" --exclude "keys/*" --exclude "install.sh" --exclude "lordos-release.pub.pem" --exclude ".well-known/*" \
  --cache-control "no-cache" --only-show-errors

id=$(aws cloudfront create-invalidation --distribution-id "$DISTRIBUTION" --paths "/*" --query Invalidation.Id --output text)
echo "published to https://$BUCKET (invalidation $id)"
