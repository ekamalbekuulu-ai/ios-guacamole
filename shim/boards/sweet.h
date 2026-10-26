#pragma once
/* Redmi Note 10 Pro (sweet, SM7150). Values match tools/devices.py. */

#define BOARD_NAME "sweet"

/* reserved-memory cont_splash_region that ABL leaves lit (live FDT). */
#define FB_BASE		0x9c000000UL
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define GICD_BASE	0x17a00000UL
#define GICR_BASE	0x17a60000UL
#define GICR_SIZE	0x100000UL
#define GICR_PE_SIZE	0x20000UL
#define VTIMER_INTID	19	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID	18	/* DT: <GIC_PPI 2 ...> */

/* SoC-level reset registers (SM7150); restart_reason magic reboots to ABL. */
#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem restart_reason@65c */
#define PSHOLD			0x0c264000UL

/* Internal UFS host (ufshc@1d84000 "ufs_mem"); probe path only. */
#define UFS_BASE		0x01d84000UL

/* Reset-surviving scratch word (DRAM): see shim/boards/guacamole.h. */
#define MARK_PA			0x118000000UL

/* The UFS probe is safe on sweet (ABL leaves the host running). */
#define PROBE_UFS		1

/* TLMM (pinctrl) base: pin stride 0x1000, ctl at +0, io at +4. */
#define BOARD_TLMM_PA		0x03900000UL
