# vmware-fusion-before-pacing — LordOS 0.8.5~rc1

The Test Bench's report on the VMware Fusion VM (see `../vmware-fusion/`) with the 0.8.5~rc1 package as
built, before software vblank pacing: the `vmwgfx` driver completes page flips at once instead of on
vblank, so the compositor drew 494–583 frames a second while dragging a window (session log) and the bench
524 frames a second (`fps` here), every one "presented", at a 60 Hz mode. A short run (no 3D, no input).
