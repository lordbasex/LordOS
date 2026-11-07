# packages-tools.sh — command-line tools in every LordOS image, the live ISO (tools/iso/build.sh) and the
# Raspberry Pi image (tools/rpi/build-img.sh), so both have the same ones. Sourced by both; the lordos package
# Recommends the same list (tools/iso/mkdeb.sh), so an update brings them to installs that lack them.
# Keep the three in step.
PKGS_TOOLS="curl wget rsync screen htop vim mc tcpdump net-tools fdisk rfkill"
