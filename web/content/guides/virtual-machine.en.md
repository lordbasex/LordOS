LordOS comes as a ready virtual machine for amd64: `lordos-<version>-amd64.ova`. It is already installed, so there is no installer to run. You open it in your hypervisor, start it and create your account.

## What you need

- A 64-bit PC (amd64) with hardware virtualization turned on in its firmware. The OVA doesn't run on computers with an ARM processor: for those, use the arm64 ISO.
- **VirtualBox 7** (tested on 7.2) or **VMware** Fusion or Workstation.
- Memory and processors for the machine: **at least 4 CPUs and 4 GB of memory**. 2 CPUs are too few for good performance. With many apps open, **16 GB is the ideal**. The computer running it needs that much free on top of its own use.
- Disk: the download is about 1.3 GB. The machine's disk is 32 GB and grows as it is used; it starts at a few GB.

All the figures for every image are in [System requirements](/requirements/).

## 1. Download and check it

Download `lordos-<version>-amd64.ova`, `SHA256SUMS` and `SHA256SUMS.sig` from the [latest release](https://github.com/lordbasex/LordOS/releases). In the folder with the files:

```sh
shasum -a 256 -c SHA256SUMS --ignore-missing
```

"OK" next to the file name means it is the file we published. Checking the signature too is explained in [Code signing](/legal/code-signing/).

## 2. Import it

### VirtualBox

File → Import Appliance, choose the `.ova` and click Finish. Under Settings you can see what the machine brings: 4 CPUs, 4 GB of memory and its display already set.

![VirtualBox's Import Virtual Appliance window](docs/vm/virtualbox-import.png "VirtualBox: Import Virtual Appliance, with the machine's settings")

### VMware Fusion

File → Import, choose the `.ova` and click Continue. Give the machine a name and a folder, then Save. When the import ends, Finish starts it.

![VMware Fusion: choosing the OVA](docs/vm/vmware-choose.png "VMware Fusion: choose the file, then Continue")

![VMware Fusion: the summary after importing](docs/vm/vmware-finish.png "VMware Fusion: the imported machine, 4 CPU cores and 4 GB of memory")

The first time it starts, Fusion may ask whether to upgrade the virtual machine. Either answer works; Upgrade is fine unless you also open it with an older version of VMware.

![VMware Fusion asks to upgrade the machine](docs/vm/vmware-upgrade.png "VMware Fusion: Upgrade or Don't Upgrade, both work")

On **VMware Workstation**: File → Open, choose the `.ova`. If VMware says the file doesn't meet the OVF specification, click Retry.

## 3. Create your account

Start the machine. It opens **Set up LordOS** by itself:

![Set up LordOS on its first start](docs/vm/virtualbox-welcome.png "The first start: Set up LordOS opens by itself")

1. **Language & region** and **Keyboard**.
2. **User**: your name, user name, password and the computer's name.

![The User step](docs/vm/virtualbox-user.png "User: your name, user name and password")

3. **Summary**, then **Create account**.

![The Summary step](docs/vm/virtualbox-summary.png "Summary, then Create account")

4. **Restart**. The login screen appears: sign in with the password you chose.

![Your account is ready](docs/vm/virtualbox-ready.png "The account is made: Restart to finish")

![The login screen after the restart](docs/vm/vmware-login.png "After the restart, the login screen (here in VMware Fusion)")

It is the same in VirtualBox and in VMware. Until the account exists, the machine runs a setup session with no password, reachable only from its own screen. Finish this step before leaving the machine running.

## Adjusting the machine

With the machine off, in its settings:

- **Processors and memory:** raise them if the host has room; 16 GB of memory is the ideal with many apps.
- **Display:** on VirtualBox, VBoxSVGA comes set; VMSVGA works too.
- **Network:** VirtualBox sets NAT, enough to browse and update; VMware Fusion picks a bridged connection, which puts the machine on your network. For other computers to reach it (remote desktop, for example), use a bridged adapter.

## If something goes wrong

- **VirtualBox says `VERR_ALREADY_EXISTS` when importing:** a folder from an earlier machine with the same name is still in `VirtualBox VMs`. Removing a machine with "Remove only" keeps its files. Change the name on the import screen, or delete that old folder.
- **A black screen or a small window:** check the display controller (above) and give the machine at least 4 GB of memory.
- **It's slow:** check that it has 4 CPUs or more and that hardware virtualization is on in the host's firmware.

For a bug report, run `lordos-issue` in the machine's Terminal and attach the file it writes to the [issue](https://github.com/lordbasex/LordOS/issues).
