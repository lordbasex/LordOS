LordOS Guest Tools
==================

This disc installs the guest agent of LordOS Machines in this Linux virtual
machine: qemu-guest-agent (a clean shutdown, the clock, the system's logs) and
lordos-guest (the clipboard, the display's size, apps mode).

Open a terminal in this machine and run install.sh from the disc, for example:

    sh /media/$USER/LORDOS_TOOLS/install.sh          (Debian, Ubuntu)
    sh /run/media/$USER/LORDOS_TOOLS/install.sh      (Fedora, openSUSE, Arch)

If the disc isn't mounted (a system without a desktop):

    sudo mount -r /dev/sr0 /mnt && sh /mnt/install.sh

The script asks for this machine's administrator password with the machine's
own prompt. Machines never sees or types it. It installs qemu-guest-agent with
the system's package manager, copies lordos-guest to /usr/local/bin and starts
both. Machines takes the disc out once the agent answers; later versions of the
tools reach this machine by themselves.

Files: install.sh (the installer), lguest (lordos-guest for the processor named
in arch.txt), sha256.txt (lguest's checksum), readme.txt (this file).
