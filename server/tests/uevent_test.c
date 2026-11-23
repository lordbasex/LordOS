/* uevent_test.c — uevent_parse (src/hw/uevent.c) on synthetic kernel datagrams: the subsystems, udev's own messages,
 * and drm uevents of a card lord-server doesn't drive (an Intel iGPU sending hotplugs as a GVT-g machine starts). */
#include "hw/uevent.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* "a|b|c" → "a\0b\0c\0", as the kernel sends it */
static int parse(const char *text) {
    char buf[1024];
    size_t n = strlen(text);
    if (n >= sizeof buf) return -1;
    for (size_t i = 0; i < n; i++) buf[i] = text[i] == '|' ? 0 : text[i];
    buf[n] = 0;
    return uevent_parse(buf, (int)n + 1);
}

#define AMD_HP "change@/devices/pci0000:00/0000:00:01.0/0000:01:00.0/drm/card1|ACTION=change|DEVPATH=/devices/pci0000:00/0000:00:01.0/0000:01:00.0/drm/card1|SUBSYSTEM=drm|HOTPLUG=1|DEVNAME=dri/card1|DEVTYPE=drm_minor|SEQNUM=5001|MAJOR=226|MINOR=1"
#define INTEL_HP "change@/devices/pci0000:00/0000:00:02.0/drm/card0|ACTION=change|DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card0|SUBSYSTEM=drm|HOTPLUG=1|DEVNAME=dri/card0|DEVTYPE=drm_minor|SEQNUM=5002|MAJOR=226|MINOR=0"
#define INTEL_CONN "change@/devices/pci0000:00/0000:00:02.0/drm/card0/card0-HDMI-A-1|ACTION=change|DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card0/card0-HDMI-A-1|SUBSYSTEM=drm|SEQNUM=5003"
#define AMD_CONN "change@/devices/pci0000:00/0000:00:01.0/0000:01:00.0/drm/card1/card1-DP-2|ACTION=change|DEVPATH=/devices/pci0000:00/0000:00:01.0/0000:01:00.0/drm/card1/card1-DP-2|SUBSYSTEM=drm|SEQNUM=5004"
#define HDR_ONLY "change@/devices/pci0000:00/0000:00:02.0/drm/card0|ACTION=change|SUBSYSTEM=drm|HOTPLUG=1|SEQNUM=5005"
#define RENDER "add@/devices/pci0000:00/0000:00:02.0/drm/renderD128|ACTION=add|DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/renderD128|SUBSYSTEM=drm|DEVNAME=dri/renderD128|SEQNUM=5006"
#define NOCARD "change@/devices/virtual/drm/ttm|ACTION=change|DEVPATH=/devices/virtual/drm/ttm|SUBSYSTEM=drm|SEQNUM=5007"
#define CARD10 "change@/devices/platform/x/drm/card10|ACTION=change|DEVPATH=/devices/platform/x/drm/card10|SUBSYSTEM=drm|HOTPLUG=1|DEVNAME=dri/card10|SEQNUM=5008"
#define LOOKALIKE "change@/devices/platform/x/drm/card1x|ACTION=change|DEVPATH=/devices/platform/x/drm/card1x|SUBSYSTEM=drm|SEQNUM=5009"

int main(void) {
    /* subsystems, every card accepted (the default) */
    CHECK(parse(AMD_HP) == UEV_DRM, "drm");
    CHECK(parse(INTEL_HP) == UEV_DRM, "drm of any card by default");
    CHECK(parse("change@/devices/LNXSYSTM:00/BAT0/power_supply/BAT0|ACTION=change|SUBSYSTEM=power_supply|POWER_SUPPLY_CAPACITY=50") == UEV_POWER, "power_supply");
    CHECK(parse("change@/devices/pci0000:00/0000:00:02.0/drm/card0/card0-eDP-1/intel_backlight|ACTION=change|SUBSYSTEM=backlight") == UEV_BACKLIGHT, "backlight (its path names card0, it still counts)");
    CHECK(parse("add@/devices/pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/video4linux/video0|ACTION=add|SUBSYSTEM=video4linux|DEVNAME=video0") == UEV_VIDEO, "video4linux (a webcam plugged in)");
    CHECK(parse("add@/devices/virtual/net/lo|ACTION=add|SUBSYSTEM=net") == 0, "net");
    CHECK(parse("add@/devices/virtual/mdev|ACTION=add|SUBSYSTEM=mdev") == 0, "mdev");
    CHECK(parse("libudev|SUBSYSTEM=drm|DEVNAME=dri/card1") == 0, "udev's own message (no '@')");
    CHECK(parse("change@/devices/x|ACTION=change") == 0, "no subsystem");
    CHECK(uevent_parse("", 0) == 0 && uevent_parse("x", -1) == 0, "empty");

    /* lord-server drives card1 (the AMD card): the Intel card0's drm uevents are ignored */
    uevent_set_card(1);
    CHECK(parse(AMD_HP) == UEV_DRM, "own card's hotplug");
    CHECK(parse(AMD_CONN) == UEV_DRM, "own card's connector");
    CHECK(parse(INTEL_HP) == 0, "another card's hotplug ignored");
    CHECK(parse(INTEL_CONN) == 0, "another card's connector ignored");
    CHECK(parse(HDR_ONLY) == 0, "another card named only in the header ignored");
    CHECK(parse(RENDER) == UEV_DRM, "a render node names no card: counts");
    CHECK(parse(NOCARD) == UEV_DRM, "no card number: counts");
    CHECK(parse(CARD10) == 0, "card10 isn't card1");
    CHECK(parse(LOOKALIKE) == UEV_DRM, "card1x isn't a card: counts");
    /* DEVNAME wins over the path */
    CHECK(parse("change@/devices/p/drm/card0|ACTION=change|DEVPATH=/devices/p/drm/card0|SUBSYSTEM=drm|DEVNAME=dri/card1") == UEV_DRM, "DEVNAME first");
    CHECK(parse("change@/devices/pci0000:00/0000:00:02.0/drm/card0/card0-eDP-1/intel_backlight|ACTION=change|SUBSYSTEM=backlight") == UEV_BACKLIGHT, "backlight isn't filtered");
    CHECK(parse("change@/devices/LNXSYSTM:00/BAT0/power_supply/BAT0|ACTION=change|SUBSYSTEM=power_supply") == UEV_POWER, "power_supply isn't filtered");

    uevent_set_card(10);
    CHECK(parse(CARD10) == UEV_DRM && parse(AMD_HP) == 0, "card10");
    uevent_set_card(-1);
    CHECK(parse(INTEL_HP) == UEV_DRM && parse(AMD_HP) == UEV_DRM, "-1 accepts every card");

    /* a DRM fd's card number (when this computer has one to open) */
    CHECK(uevent_card_of_fd(-1) == -1, "no fd");
    int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(uevent_card_of_fd(nul) == -1, "/dev/null isn't a card");
    if (nul >= 0) close(nul);
    for (int i = 0; i < 4; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/dri/card%d", i);
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        CHECK(uevent_card_of_fd(fd) == i, "%s: card %d", p, uevent_card_of_fd(fd));
        close(fd);
    }
    int r = open("/dev/dri/renderD128", O_RDONLY | O_CLOEXEC);
    if (r >= 0) { CHECK(uevent_card_of_fd(r) == -1, "a render node isn't a card"); close(r); }

    printf("uevent-test: %s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
