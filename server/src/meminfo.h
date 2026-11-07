/* meminfo.h — the memory "in use" from /proc/meminfo, counted the same way by Monitor, About, Settings and
 * lordos-sysreport.
 *
 * In use = MemTotal − MemFree − (Buffers + Cached + SReclaimable − Shmem): what programs and the kernel hold
 * and can't give back, the way htop counts it. The cache (files read, reclaimable kernel caches) is apart;
 * shared memory (Shmem, tmpfs) is counted in Cached but can't be dropped, so it counts as in use.
 *
 * Not MemTotal − MemAvailable: MemAvailable also leaves out the kernel's reserve of free pages (its zone
 * watermarks), which grows with the RAM and holds nothing. With 128 GB that was ~0.85 GB shown as in use
 * (2.3 GB in Monitor for 1.4 GB really used); with 4 GB on a Raspberry Pi it is a few tens of MB. */
#ifndef LORD_MEMINFO_H
#define LORD_MEMINFO_H

/* Buffers + Cached + SReclaimable − Shmem (never below 0), in the units given. */
static inline unsigned long long meminfo_cache(unsigned long long buffers, unsigned long long cached,
                                               unsigned long long sreclaimable, unsigned long long shmem) {
    unsigned long long c = buffers + cached + sreclaimable;
    return c > shmem ? c - shmem : 0;
}

/* The memory in use, in the units given (between 0 and total). */
static inline unsigned long long meminfo_used(unsigned long long total, unsigned long long free, unsigned long long buffers,
                                              unsigned long long cached, unsigned long long sreclaimable,
                                              unsigned long long shmem) {
    unsigned long long held = free + meminfo_cache(buffers, cached, sreclaimable, shmem);
    return total > held ? total - held : 0;
}

#endif
