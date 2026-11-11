#!/bin/sh
# One-time CloudFront change so lordos.org can serve the APT repository (docs/UPDATES.md,
# "Serving it"): two cache behaviors, apt/* and apt-staging/*, copied from the default one but
# serving plain HTTP as is (allow-all: LordOS's package engine reads https sources over http and
# trusts the signatures, like apt with deb.debian.org) and without the lordos-router function
# (it would turn InRelease, Release and Packages into <path>/index.html).
#
#   tools/web/cdn-apt-behaviors.sh <aws-profile-that-can-update-the-distribution>
#
# The deploy profile cannot change the distribution; run this with the account's
# administrator profile. Idempotent: running it again replaces the two behaviors with fresh copies.
# Then check with: tools/release/apt-repo.sh verify staging
set -eu
PROFILE=${1:?usage: cdn-apt-behaviors.sh <aws-profile>}
. "$(cd "$(dirname "$0")" && pwd)/aws-env.sh"
lordos_aws_env LORDOS_CF_DISTRIBUTION
DISTRIBUTION=$LORDOS_CF_DISTRIBUTION
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
aws --profile "$PROFILE" cloudfront get-distribution-config --id "$DISTRIBUTION" > "$t/current.json"
python3 - "$t" <<'PY'
import copy, json, sys
t = sys.argv[1]
d = json.load(open(f"{t}/current.json"))
c = d["DistributionConfig"]
keep = [b for b in c.get("CacheBehaviors", {}).get("Items", []) or [] if b["PathPattern"] not in ("apt/*", "apt-staging/*")]
for pattern in ("apt/*", "apt-staging/*"):
    b = copy.deepcopy(c["DefaultCacheBehavior"])
    b["PathPattern"] = pattern
    b["ViewerProtocolPolicy"] = "allow-all"
    b["FunctionAssociations"] = {"Quantity": 0}
    b["Compress"] = False
    keep.append(b)
c["CacheBehaviors"] = {"Quantity": len(keep), "Items": keep}
json.dump(c, open(f"{t}/new.json", "w"))
open(f"{t}/etag", "w").write(d["ETag"])
PY
aws --profile "$PROFILE" cloudfront update-distribution --id "$DISTRIBUTION" --if-match "$(cat "$t/etag")" \
    --distribution-config "file://$t/new.json" --query Distribution.Status --output text
echo "CloudFront is deploying the change (a few minutes); then: tools/release/apt-repo.sh verify staging"
