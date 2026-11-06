#!/bin/sh
# push.sh (on the Mac): rebuild the e2e bundle and copy the bench to VM 116; with "full" also the tree, make, install.
set -e
cd "$(dirname "$0")/../../.."
K="-i $HOME/.ssh/ssh/sshkey/lord.basex.pem"; VM=lord@172.17.0.52
node tools/cloud/e2e/build-e2e.mjs
if [ "${1:-}" = full ]; then
    node tools/build-theme.mjs >/dev/null; node tools/cloud/e2e/build-e2e.mjs
    rsync -a --delete -e "ssh $K" --exclude .git --exclude node_modules --exclude server/build --exclude /out ./ $VM:LordOS-cloud-e2e/
    ssh $K $VM 'cd ~/LordOS-cloud-e2e/server && make -j8 > ~/e2e-make.log 2>&1 && echo make ok'
else
    rsync -a -e "ssh $K" tools/cloud/e2e/ $VM:LordOS-cloud-e2e/tools/cloud/e2e/
    rsync -a -e "ssh $K" themes/default/theme.e2e.bundle.js themes/default/code-manifest.e2e.json $VM:LordOS-cloud-e2e/themes/default/
fi
ssh $K $VM 'sudo sh ~/LordOS-cloud-e2e/tools/cloud/e2e/session.sh install /home/lord/LordOS-cloud-e2e >/dev/null && echo installed'
