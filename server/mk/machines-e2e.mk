# Machines' end-to-end harness (tools/vm/e2e/machines/README.md): a headless lord-server-test with the real default theme
# in private user and mount namespaces, a scratch library, the Machines app driven through the test hooks (the remote
# desktop's own injection) and checked on screen and on disk, real guests booted under KVM. Not in `make test`: it needs
# KVM, the network once (the guest image is downloaded and prepared in /var/tmp/lordos-m100-images) and several minutes.
# Each target says "skipped: …" and exits 0 when this computer can't run it.
#   make e2e-machines [E2E_FLOWS="library assistant …"]   the app's flows (tools/vm/e2e/machines/flows.py)
#   make e2e-machines-matrix [E2E_ROWS=kvm-uefi-2d,…]       the boot matrix (boot-matrix.py)
#   make e2e-machines-image                                 only the guest image (images.py --agent)
E2E_MH := ../tools/vm/e2e/machines
.PHONY: e2e-machines e2e-machines-matrix e2e-machines-image
e2e-machines-image: build/lord-vm
	@python3 -I $(E2E_MH)/images.py --agent; s=$$?; [ $$s = 0 ] || [ $$s = 77 ] || exit 1
e2e-machines: build/lord-server-test build/lord-vm e2e-machines-image
	@python3 -I $(E2E_MH)/run.py $(E2E_FLOWS); s=$$?; [ $$s = 0 ] || [ $$s = 77 ] || exit 1
e2e-machines-matrix: build/lord-vm e2e-machines-image
	@python3 -I $(E2E_MH)/boot-matrix.py $(if $(E2E_ROWS),--rows $(E2E_ROWS)); s=$$?; [ $$s = 0 ] || [ $$s = 77 ] || exit 1
# the release gates (disk integrity, failure recovery, leaks: tools/vm/bare-metal-gates.sh) with this build's lord-vm and
# the harness's cached Debian image, through ns-run.sh (a private home and runtime folder), so they never see the person's
# machines or session. About 15 minutes; the results file goes to build/. Exit 2 from the script (this computer can't
# run them: a virtual machine, no VT-x) is a skip.
.PHONY: e2e-machines-gates
GATES_ARCH := $(subst x86_64,amd64,$(subst aarch64,arm64,$(shell uname -m)))
e2e-machines-gates: build/lord-vm
	@python3 -I $(E2E_MH)/images.py >/dev/null; s=$$?; [ $$s = 0 ] || { echo "skipped: no guest image (images.py)"; exit 0; }; \
	sh $(E2E_MH)/ns-run.sh sh ../tools/vm/bare-metal-gates.sh --lord-vm build --image /var/tmp/lordos-m100-images/debian13-$(GATES_ARCH).qcow2 \
		--work /var/tmp $(if $(GATES_CYCLES),--cycles $(GATES_CYCLES)) --out build/machines-gates.txt; s=$$?; [ $$s = 0 ] || [ $$s = 2 ] || [ $$s = 77 ] || exit 1
