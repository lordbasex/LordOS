// LordBench's reference machines (docs/BENCHMARK.md, "Results without one magic number"): a result is
// shown as "×N" of each one measured with the same LordBench version. Every entry is a real Full run,
// copied from the lordbench.json it wrote, with the machine described in sysinfo.perf.ref.<id>. A measurement
// added to LordBench after a reference's run is listed in its notMeasured until that machine is measured again.
// Planned: a modest x86 PC; it is added when it has been measured.
export const REFERENCES = [
  {
    // VM 116 on Proxmox VE: 8 vCPU (host CPU, Xeon Silver 4108 at 1.8 GHz), 4 GB, virtio disk, no
    // GPU passthrough (Mesa llvmpipe); LordOS 0.8.6 in development, 2026-10-27, Full with 30 s sustained
    id: 'vm', lordbench: 1, mode: 'full',
    results: {
      'cpu.sha256.1': { value: 165.951 }, 'cpu.sha256.n': { value: 984.265 },
      'cpu.matmul.1': { value: 3.31858 }, 'cpu.matmul.n': { value: 19.9376 },
      'cpu.xz.1': { value: 60.2491 }, 'cpu.xz.n': { value: 362.977 },
      'cpu.js.1': { value: 56.6665 }, 'cpu.js.n': { value: 335.945 },
      'mem.copy': { value: 20.0074 }, 'mem.triad': { value: 22.8006 },
      'mem.latency': { value: 90.9222 }, 'gpu.scene': { value: 802.066 },
      'gpu.fill': { value: 406.961 }, 'gpu.upload': { value: 9456.65 },
      'gpu.layers': { value: 22.925 }, 'disk.seqwrite': { value: 378.126 },
      'disk.seqread': { value: 516.647 }, 'disk.random4k': { value: 6256.32 },
      'sustained.start': { value: 19.5814 }, 'sustained.end': { value: 19.77 },
      'video.h264.1080': { value: 399.438 }, 'video.hevc.1080': { value: 385.477 },
      'video.vp9.1080': { value: 215.005 }, 'video.av1.1080': { value: 200.887 },
      'video.h264.2160': { value: 75.624 }, 'video.hevc.2160': { value: 102.252 },
      'video.enc.h264': { value: 106.12 }, 'video.enc.vp8': { value: 54.175 },
      // the layout, paint and shaping workloads, from a later Full run of the same machine (2026-10-28, no
      // sustained or storage part; its other measurements were within 4 % of the ones above). They time LordOS's
      // engine too, so each carries the LordOS version the run reported and is compared only with results of that
      // version (perf.js ENGINE_IDS): measure them again for every release
      'cpu.ui.1': { value: 36.9702, lordos: '0.8.5-beta' }, 'cpu.ui.n': { value: 39.9302, lordos: '0.8.5-beta' },
      'cpu.shape.1': { value: 618.791, lordos: '0.8.5-beta' }, 'cpu.shape.n': { value: 3595.11, lordos: '0.8.5-beta' },
    },
  },
  {
    // Raspberry Pi 5 Model B Rev 1.1, 16 GB, a 32 GB SD card, the LordOS kernel 6.18.57-lordos.3, the fan on
    // its header (lord-fand), GPU V3D 7.1; LordOS 0.8.5 with LordBench 1 from the 0.8.6 tree run from a
    // terminal, 2026-10-27, Full with 180 s sustained (55.6 °C at most, no speed lost)
    id: 'pi5', lordbench: 1, mode: 'full',
    results: {
      'cpu.sha256.1': { value: 933.43 }, 'cpu.sha256.n': { value: 3692.78 },
      'cpu.matmul.1': { value: 2.26049 }, 'cpu.matmul.n': { value: 5.98807 },
      'cpu.xz.1': { value: 66.9597 }, 'cpu.xz.n': { value: 254.003 },
      'cpu.js.1': { value: 54.342 }, 'cpu.js.n': { value: 181.428 },
      'mem.copy': { value: 7.61055 }, 'mem.triad': { value: 7.29512 },
      'mem.latency': { value: 114.505 }, 'gpu.scene': { value: 795.282 },
      'gpu.fill': { value: 2454.68 }, 'gpu.upload': { value: 3897.05 },
      'gpu.layers': { value: 115.042 }, 'disk.seqwrite': { value: 65.9909 },
      'disk.seqread': { value: 90.5924 }, 'disk.random4k': { value: 2692.31 },
      'sustained.start': { value: 5.85622 }, 'sustained.end': { value: 5.87865 },
      'video.h264.1080': { value: 257.551 }, 'video.hevc.1080': { value: 246.029 },
      'video.vp9.1080': { value: 204.453 }, 'video.av1.1080': { value: 159.402 },
      'video.h264.2160': { value: 55.007 }, 'video.hevc.2160': { value: 57.213 },
      'video.enc.h264': { value: 75.332 }, 'video.enc.vp8': { value: 54.526 },
      // the layout, paint and shaping workloads, from a later Full run of the same board (2026-10-30, the arm64
      // build of main from a terminal, 180 s sustained, ondemand governor, 37.0 -> 49.6 °C, 59 °C at most; a
      // second Full run without the sustained part gave the same within 0.4 %). Compared only with results of the
      // same LordOS version (perf.js ENGINE_IDS): measure them again for every release
      'cpu.ui.1': { value: 39.2238, lordos: '0.8.5-beta' }, 'cpu.ui.n': { value: 45.2309, lordos: '0.8.5-beta' },
      'cpu.shape.1': { value: 628.321, lordos: '0.8.5-beta' }, 'cpu.shape.n': { value: 2514.02, lordos: '0.8.5-beta' },
    },
  },
];
