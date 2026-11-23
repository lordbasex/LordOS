---
title: Monitor
app: monitor
section: apps
order: 26
keywords: monitor, processes, cpu, memory, network, connections, disks, map, gpu, graphics card, vram, fan, ai
---
# Monitor

Shows what is going on in the computer: processor, memory, network, disks, processes and each
app's network connections.

- **CPU, memory and disks:** live graphs, per core and per process.
- **Processes:** sorted by any column, with search, filters by origin and by program. **Grouped** shows one row per app with its processes inside, and **Quit All** ends a whole app at once. Each process shows who started it and a badge of where it comes from (a blue seal for LordOS itself, a pink one for LordOS apps), and Statistics adds averages and peaks over 1, 5 or 15 minutes. While the pointer is over the list its order stays still.
- **Network:** one row per app with its traffic and connections, and a **map** with arcs to each
  server's country. The country comes from a local database: nobody is asked.
- **GPU:** each graphics card drawn live, its load, temperature, fan, memory, clocks and power, the apps using it
  (and which graphics APIs they use), the APIs installed, and whether LordOS's voice is running on it. An
  administrator can set an AMD card's fan by hand; it goes back to automatic if the card gets too hot and when
  you log out.
- **Privacy:** which apps used the camera, microphone or screen.

When closed it costs nothing: data is only collected while the window is open.
