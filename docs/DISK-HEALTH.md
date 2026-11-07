# Disk life

Design for the next version (after 0.8.8.5). Status: **designed, not built**. Tick the checklist at the end as parts land.

Goal: every disk shows **how much life it has left**, the way a laptop shows its battery's health: one number with a heart,
a plain word (Good, Worn, Replace soon, Failing), and the facts behind it (written terabytes, hours, temperature, spare
blocks). It is shown in **Monitor → Disks** (a heart per disk, next to the temperature) and in the **Disks** app (a *Life*
card in the disk's detail). Same on amd64 and arm64 PCs and on the Raspberry Pi 5 (an M.2 NVMe drive on a HAT, the SD
card, USB disks).

## 1. What already exists (0.8.8.5)

- **LordOS's own SMART reader** in C, no external program: `disk-smart <disk>` in the privileged helper
  (`server/src/disk/disk_admin.c`): ATA SMART through `SG_IO` with ATA PASS-THROUGH (16), and the NVMe health log (log page
  02h) through `NVME_IOCTL_ADMIN_CMD`; parsers in `server/src/disk/` (`smart_nvme_parse`, tested in `smartctl_test`).
- **smartmontools optional**: with Debian's `smartctl` installed, Disks shows the full report (attributes, error log, SCT
  temperature history, self-tests); without it, the own reader gives the basic health (`apps/disks/health.jsx`).
- **Live temperature without root** from the kernel's hwmon sensors (the `nvme` driver, `drivetemp` for SATA):
  `lord.monitor`, Monitor → Disks (`apps/monitor/DisksTab.jsx`), with a history graph and alerts.
- **A health light** (OK / Check / Failing) from a cached SMART summary, refreshed at most every 5 minutes per disk, never
  waking a resting disk.
- **Wear** is already read where the disk reports it (`m.wear.used`), but only shown as one line of the SMART card.

So the two speeds the request asks for already exist in spirit (temperature live from hwmon, health from a cached,
rare query). What's missing is the **life model**, its **history**, the **heart** in both apps, and the disks the reader
doesn't cover yet (cards, eMMC, many USB bridges).

## 2. Two speeds, never one loop

- **Temperature: every 2 s while Monitor is open**, from hwmon (`/sys/class/hwmon/*/temp1_input`). It's a sysfs read of a
  value the driver already keeps: no command reaches the disk. The disk itself is only asked when there's no hwmon sensor,
  and then no faster than every 60 s (NVMe log 02h has the temperature too; ATA SCT status).
- **Life: every 6 hours** (and when the Disks detail or Monitor's heart is opened, if the last reading is older than 1
  hour), in the privileged helper, **one disk at a time**, never while the disk sleeps (ATA CHECK POWER MODE first; NVMe in a
  deep power state is left alone), never in the background on battery. Each reading is a few hundred bytes: the cost is
  one admin command, not I/O.
- **Why not faster:** health logs are admin commands that queue with the disk's I/O, can wake it and warm it, and their
  numbers only move over days. Reading them every second would add stutter and heat and give nothing.

## 3. The life number, per kind of disk

`life` is 0–100 % (100 = new), from what the disk itself reports; LordOS never invents it. Where the disk reports no wear,
the card says so and shows the condition instead of a number.

| Disk | Source | Life | Also shown |
|---|---|---|---|
| **NVMe** (PC, Pi 5 HAT) | Log 02h | `100 − Percentage Used` (clamped to 0; it can pass 100) | Available Spare vs its threshold, Critical Warning bits, Media and Data Integrity Errors, Data Units Written (× 512 000 bytes = written TB), Power On Hours, Unsafe Shutdowns, temperature and the time spent above the warning and critical limits |
| **SATA SSD** | ATA Device Statistics (GP log 04h): page 07h *Percentage Used Endurance Indicator* (standard, ACS-3); page 01h *Logical Sectors Written*, *Power-On Hours* | `100 − Percentage Used` | Reallocated, pending and uncorrectable counts; when page 07h is missing, the vendor attributes as a fallback, each normalized value read the way the vendor documents: 177 Wear Leveling Count, 231 SSD Life Left, 233 Media Wearout Indicator, 202 Percent Lifetime Remaining; written bytes from 241 Total LBAs Written only when its unit is known |
| **Hard disk (HDD)** | ATA SMART + Device Statistics | **no wear %**: platters don't wear like flash | Condition from reallocated, pending and uncorrectable sectors (any growth is *Check*, pending > 0 is *Replace soon*), Power-On Hours, start/stop and load cycles, temperature, SMART overall result |
| **eMMC** (boards, some laptops) | sysfs, no root: `/sys/block/mmcblk*/device/life_time` (EXT_CSD *DEVICE_LIFE_TIME_EST_TYP_A/B*, in steps of 10 %) and `pre_eol_info` (normal / 80 % of spare used / urgent) | `100 − 10 × (worse of A and B − 1)` | Pre-EOL state, model and serial from sysfs |
| **SD card** (Raspberry Pi) | No standard health. sysfs gives identity: `/sys/block/mmcblk0/device/{name,serial,manfid,oemid,date,type}` | **"This card doesn't report its life"** | Model, maker, serial and date from the card's CID (today the Disks screen shows "—" for model and serial on the Pi's card: fix that); speed mode and clock (`/sys/kernel/debug/mmc0/ios`, root, read once); LordOS's own count of what was written to it since it was installed (from `/sys/block/mmcblk0/stat`, kept in the history), and a note that cards wear out with writes |
| **USB disks** | SAT pass-through (`SG_IO` ATA 16) works through most bridges; NVMe-to-USB bridges need their vendor pass-through (some bridges map NVMe admin commands; the rest don't) | As the disk inside, when the bridge lets it through | Otherwise "This disk's adapter doesn't pass its health on" |
| Virtual disks, images | — | Not shown | — |

**Remaining life in time.** No disk reports its rated endurance (TBW) in its logs, so LordOS estimates from the disk's own
wear and how fast it is writing: with `used %` and `written TB` read at two dates at least 7 days apart, *projected
total ≈ written ÷ used × 100*, and *years left ≈ (100 − used) ÷ (used per day) ÷ 365*. Shown only after 7 days of history,
as "about N years at this rate", never as a promise. The person can also type the rated TBW from the box or spec sheet
(per disk, stored with its serial); then the card also shows "written X of Y TB".

**The word and the heart color:**

| Life | Word | Heart |
|---|---|---|
| ≥ 80 % and no warning | Good | green |
| 30–79 % | Worn (normal use) | green, partly empty |
| 10–29 % | Replace soon | orange |
| < 10 %, or Available Spare below its threshold, or a Critical Warning bit, or SMART overall FAILED, or pending sectors | Replace now / Failing | red |

The heart fills to the life level (like a battery icon). A disk without a life number shows an outline heart with its
condition word.

## 4. History

- One point per disk per day, kept as root in `/var/lib/lordos/disk-health/<serial-hash>.json` (life, written bytes,
  power-on hours, max temperature of the day, errors): about 40 bytes a day, 15 KB a year. Keyed by serial (hashed), so a disk
  moved to another port or another LordOS computer keeps its history on that computer; never sent anywhere.
- Disks → the *Life* card shows the curve of life and of written TB over time, and the estimate above.
- The temperature history of Monitor stays as it is (live, in memory).

## 5. The screens

- **Monitor → Disks:** each disk row gets a heart with its life % (or outline + word), next to temperature and the health
  light; the chosen disk's panel adds the *Life* summary (life %, written TB, hours, estimate) and *Open in Disks*.
- **Disks → the disk's detail:** a **Life** card above *Disk health (SMART)*: big heart and %, the word, *written N TB*
  (and *of M TB* if the rated TBW was given), *on for N hours*, temperature now and its limits, spare blocks, the
  estimate, the history curve and *Set rated endurance…*. The SMART card stays below for the full report.
- **Speeds** (the request names them): the negotiated link next to Connection: PCIe generation and lanes from
  `/sys/class/nvme/nvme*/device/current_link_{speed,width}` against the maximum (`max_link_*`), SATA link speed from
  `/sys/class/ata_link/*/sata_spd`, USB speed from the device's `speed`, SD bus mode from the card; live read and write rates
  are already in Monitor. A slower link than the disk supports is said ("running at PCIe 3.0 ×2 of 4.0 ×4").
- **Notifications** (once per crossing, settings in Monitor's alerts): life below 20 % and 10 %, Available Spare below
  threshold, a new Critical Warning bit, new reallocated or pending sectors, temperature above the disk's critical limit.
- **The SD card fix seen on the Pi** (photo 2026-11-07): the SMART card says "Not checked yet" and "does not report its
  health" at once. A card should say only "This card doesn't report its life" and show the card's identity and the written
  count.

## 6. Security and cost

- Reading stays in the privileged helper's `disk-smart` (and a new `disk-life`) with the same rules: a real block device by
  name and number, read-only admin commands only (Get Log Page, Identify, SMART READ DATA / READ LOG EXT, CHECK POWER MODE);
  no vendor-specific or write commands, ever. The parsers are pure functions over fixed-size buffers, fuzzed like the
  others.
- sysfs values (eMMC life, SD identity, link speeds, hwmon) need no root.
- CPU: a reading is microseconds of parsing; the only cost is the admin command, every 6 hours.

## 7. Tests

- Parser tests from recorded buffers (NVMe log 02h, ATA Device Statistics pages 01h and 07h, SMART attribute tables of
  several vendors, eMMC sysfs values), including out-of-range values (Percentage Used above 100, missing pages).
- The estimate and the word thresholds as a JS model test.
- On hardware: the maintainer's i9 PC with a new NVMe (life near 100 %, a few TB written) and its SATA disks; the Pi 5 with its
  SD card and, if present, an NVMe HAT; a USB SSD through a SAT bridge and an NVMe USB enclosure.

## 8. Checklist

- [ ] `disk-life` in the helper: NVMe log 02h (all fields above), ATA Device Statistics 01h/07h, vendor wear attributes as
  fallback, CHECK POWER MODE before any ATA read.
- [ ] eMMC life from sysfs; SD card identity from sysfs (fix the "—" model and serial).
- [ ] Link speeds (PCIe, SATA, USB, SD bus mode).
- [ ] The scheduler: every 6 h, one disk at a time, skipped while asleep or on battery; history file per disk.
- [ ] Estimate of remaining life (after 7 days) and the optional rated TBW per disk.
- [ ] Monitor → Disks: heart per disk and the Life summary.
- [ ] Disks: the Life card with the curve; the SD card wording fix.
- [ ] Notifications.
- [ ] i18n (en, es, pt, fr, it) and docs (DISKS section of the user docs, MONITOR.md).
- [ ] Tests (parsers, model) and the hardware checks of section 7.
