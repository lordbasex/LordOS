/** @file
  Register offsets and fields used by the LordOS GVT-g GOP driver.

  Gen9 (Skylake, Kaby Lake, Coffee Lake, Comet Lake) display engine as the
  Linux GVT-g mediator emulates it for a vGPU. Offsets and bit fields are taken
  from the MIT-licensed Linux headers listed in NOTICE.md (i915_reg.h,
  i915_pvinfo.h, intel_dp_aux_regs.h, skl_universal_plane_regs.h,
  intel_gt_regs.h).

  Copyright (c) 2026 iPERFEX and the LordOS contributors
  SPDX-License-Identifier: MIT
**/

#ifndef GVT_REGS_H_
#define GVT_REGS_H_

//
// BAR 0 (GTTMMADR) layout on gen8+: 2 MB of MMIO registers, the global GTT
// (one 64-bit entry per 4 KB page) from 8 MB.
//
#define GVT_MMIO_SIZE          SIZE_2MB
#define GVT_GGTT_OFFSET        SIZE_8MB
#define GVT_GGTT_PTE_PRESENT   BIT0
#define GVT_GGTT_ADDR_MASK     0x0000007FFFFFF000ULL   // bits 38:12

//
// PVINFO page: the paravirtual interface a GVT-g vGPU exposes at 0x78000.
//
#define PVINFO_BASE                 0x78000
#define PVINFO_MAGIC_LO             (PVINFO_BASE + 0x00)
#define PVINFO_MAGIC_HI             (PVINFO_BASE + 0x04)
#define PVINFO_VERSION              (PVINFO_BASE + 0x08)  // major 15:0, minor 31:16
#define PVINFO_VGT_ID               (PVINFO_BASE + 0x0C)
#define PVINFO_MAPPABLE_BASE        (PVINFO_BASE + 0x40)
#define PVINFO_MAPPABLE_SIZE        (PVINFO_BASE + 0x44)
#define PVINFO_NONMAPPABLE_BASE     (PVINFO_BASE + 0x48)
#define PVINFO_NONMAPPABLE_SIZE     (PVINFO_BASE + 0x4C)
#define PVINFO_MAGIC                0x4776544776544776ULL   // "vGTvGTvG"
#define PVINFO_VERSION_MAJOR        1

//
// Global GTT TLB flush.
//
#define GFX_FLSH_CNTL_GEN6          0x101008
#define GFX_FLSH_CNTL_EN            BIT0

//
// VGA plane.
//
#define VGACNTRL                    0x41000
#define VGACNTRL_DISP_DISABLE       BIT31

//
// Power wells (HSW_PWR_WELL_CTL2, the driver's request register).
// Request bit = 2 << (2 * index), state bit = 1 << (2 * index).
//
#define PWR_WELL_CTL2               0x45404
#define PWR_WELL_REQ(Idx)           (BIT1 << ((Idx) * 2))
#define PWR_WELL_STATE(Idx)         (BIT0 << ((Idx) * 2))
#define PW_IDX_DDI_A_E              1
#define PW_IDX_DDI_B                2
#define PW_IDX_DDI_C                3
#define PW_IDX_DDI_D                4
#define PW_IDX_PW_1                 14
#define PW_IDX_PW_2                 15

//
// Display PLLs (Skylake family). DPLL0 also feeds CDCLK, so a port gets DPLL1.
//
#define LCPLL1_CTL                  0x46010
#define LCPLL2_CTL                  0x46014
#define LCPLL_PLL_ENABLE            BIT31
#define DPLL_CTRL1                  0x6C058
#define DPLL_CTRL1_ID_MASK(Id)      (0x3FU << ((Id) * 6))
#define DPLL_CTRL1_OVERRIDE(Id)     (1U << ((Id) * 6))
#define DPLL_CTRL1_LINK_RATE(R, Id) ((UINT32)(R) << ((Id) * 6 + 1))
#define DPLL_CTRL1_LINK_RATE_2700   0   // HBR2, 5.4 Gb/s per lane
#define DPLL_CTRL2                  0x6C05C
#define DPLL_CTRL2_DDI_CLK_OFF(P)   (1U << ((P) + 15))
#define DPLL_CTRL2_DDI_CLK_SEL_MASK(P)   (3U << ((P) * 3 + 1))
#define DPLL_CTRL2_DDI_CLK_SEL(Clk, P)   ((UINT32)(Clk) << ((P) * 3 + 1))
#define DPLL_CTRL2_DDI_SEL_OVERRIDE(P)   (1U << ((P) * 3))
#define DPLL_STATUS                 0x6C060
#define DPLL_LOCK(Id)               (1U << ((Id) * 8))
#define GVT_PORT_DPLL               1
#define GVT_LINK_CLOCK_KHZ          540000   // port clock for HBR2
#define GVT_LINK_LANES              4

//
// DDI ports. Port index: A = 0, B = 1, C = 2, D = 3, E = 4.
//
#define PORT_A                      0
#define PORT_B                      1
#define PORT_C                      2
#define PORT_D                      3
#define PORT_E                      4
#define DDI_BUF_CTL(P)              (0x64000 + (P) * 0x100)
#define DDI_BUF_CTL_ENABLE          BIT31
#define DDI_BUF_IS_IDLE             BIT7
#define DDI_PORT_WIDTH(L)           ((UINT32)(((L) == 3) ? 4 : ((L) - 1)) << 1)
#define DP_TP_CTL(P)                (0x64040 + (P) * 0x100)
#define DP_TP_CTL_ENABLE            BIT31
#define DP_TP_CTL_MODE_SST          0
#define DP_TP_CTL_ENHANCED_FRAME    BIT18
#define DP_TP_CTL_TRAIN_MASK        (7U << 8)
#define DP_TP_CTL_TRAIN_PAT1        (0U << 8)
#define DP_TP_CTL_TRAIN_PAT2        (1U << 8)
#define DP_TP_CTL_TRAIN_IDLE        (2U << 8)
#define DP_TP_CTL_TRAIN_NORMAL      (3U << 8)
#define DP_TP_STATUS(P)             (0x64044 + (P) * 0x100)
#define DP_TP_STATUS_IDLE_DONE      BIT25

//
// DP AUX channel (one per port), five 32-bit data registers after CTL.
//
#define DP_AUX_CH_CTL(P)            (0x64010 + (P) * 0x100)
#define DP_AUX_CH_DATA(P, N)        (0x64014 + (P) * 0x100 + (N) * 4)
#define DP_AUX_CTL_SEND_BUSY        BIT31
#define DP_AUX_CTL_DONE             BIT30
#define DP_AUX_CTL_INTERRUPT        BIT29
#define DP_AUX_CTL_TIME_OUT_ERROR   BIT28
#define DP_AUX_CTL_TIME_OUT_MAX     (3U << 26)
#define DP_AUX_CTL_RECEIVE_ERROR    BIT25
#define DP_AUX_CTL_MSG_SIZE_SHIFT   20
#define DP_AUX_CTL_MSG_SIZE_MASK    (0x1FU << 20)
#define DP_AUX_CTL_FW_SYNC_PULSE(C) ((UINT32)((C) - 1) << 5)
#define DP_AUX_CTL_SYNC_PULSE(C)    ((UINT32)((C) - 1))

//
// AUX request codes (DisplayPort 1.2, section 2.7) and DPCD addresses.
//
#define AUX_I2C_WRITE               0x0
#define AUX_I2C_READ                0x1
#define AUX_I2C_MOT                 0x4
#define AUX_NATIVE_WRITE            0x8
#define AUX_NATIVE_READ             0x9
#define DPCD_LINK_BW_SET            0x100
#define DPCD_LANE_COUNT_SET         0x101
#define DPCD_TRAINING_PATTERN_SET   0x102
#define DPCD_SET_POWER              0x600
#define DPCD_LINK_BW_5_4            0x14
#define DPCD_LANE_COUNT_ENHANCED    0x80
#define DPCD_TRAINING_PAT1_NOSCR    0x21
#define DPCD_TRAINING_PAT2_NOSCR    0x22
#define EDID_I2C_ADDR               0x50

//
// Transcoder / pipe A. Every timing field holds the value minus one.
//
#define TRANS_HTOTAL_A              0x60000
#define TRANS_HBLANK_A              0x60004
#define TRANS_HSYNC_A               0x60008
#define TRANS_VTOTAL_A              0x6000C
#define TRANS_VBLANK_A              0x60010
#define TRANS_VSYNC_A               0x60014
#define PIPESRC_A                   0x6001C
#define PIPE_DATA_M1_A              0x60030
#define PIPE_DATA_N1_A              0x60034
#define PIPE_LINK_M1_A              0x60040
#define PIPE_LINK_N1_A              0x60044
#define PIPE_TU_SIZE(X)             ((UINT32)((X) - 1) << 25)
#define TRANS_DDI_FUNC_CTL_A        0x60400
#define TRANS_DDI_FUNC_ENABLE       BIT31
#define TRANS_DDI_PORT_SHIFT        28
#define TRANS_DDI_PORT_MASK         (7U << 28)
#define TRANS_DDI_MODE_DP_SST       (2U << 24)
#define TRANS_DDI_BPC_8             (0U << 20)
#define TRANS_DDI_PVSYNC            BIT17
#define TRANS_DDI_PHSYNC            BIT16
#define TRANS_DDI_PORT_WIDTH(L)     ((UINT32)((L) - 1) << 1)
#define TRANSCONF_A                 0x70008
#define TRANSCONF_ENABLE            BIT31
#define TRANSCONF_STATE_ENABLE      BIT30

//
// Universal plane 1 of pipe A (the primary plane).
//
#define PLANE_CTL_1_A               0x70180
#define PLANE_STRIDE_1_A            0x70188
#define PLANE_POS_1_A               0x7018C
#define PLANE_SIZE_1_A              0x70190
#define PLANE_SURF_1_A              0x7019C
#define PLANE_OFFSET_1_A            0x701A4
#define PLANE_CTL_ENABLE            BIT31
#define PLANE_CTL_FORMAT_XRGB_8888  (4U << 24)
#define PLANE_CTL_TILED_LINEAR      (0U << 10)
#define PLANE_CTL_ALPHA_DISABLE     (0U << 4)
#define PLANE_STRIDE_ALIGN          64

#endif
