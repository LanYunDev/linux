/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Renesas Mobile SDHI
 *
 * Copyright (C) 2017 Horms Solutions Ltd., Simon Horman
 * Copyright (C) 2017-19 Renesas Electronics Corporation
 */

#ifndef RENESAS_SDHI_H
#define RENESAS_SDHI_H

#include <linux/dmaengine.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include "tmio_mmc.h"

struct renesas_sdhi_scc {
	unsigned long clk_rate;	/* clock rate for SDR104 */
	u32 tap;		/* sampling clock position for SDR104/HS400 (8 TAP) */
	u32 tap_hs400_4tap;	/* sampling clock position for HS400 (4 TAP) */
};

#define SDHI_FLAG_NEED_CLKH_FALLBACK	BIT(0)

struct renesas_sdhi_of_data {
	unsigned long tmio_flags;
	u32	      tmio_ocr_mask;
	unsigned long capabilities;
	unsigned long capabilities2;
	enum dma_slave_buswidth dma_buswidth;
	dma_addr_t dma_rx_offset;
	unsigned int bus_shift;
	int scc_offset;
	struct renesas_sdhi_scc *taps;
	int taps_num;
	unsigned int max_blk_count;
	unsigned short max_segs;
	unsigned long sdhi_flags;
};

#define SDHI_CALIB_TABLE_MAX 32

#define sdhi_has_quirk(p, q) ((p)->quirks && (p)->quirks->q)

struct renesas_sdhi_quirks {
	bool hs400_disabled;
	bool hs400_4taps;
	bool fixed_addr_mode;
	bool dma_one_rx_only;
	bool manual_tap_correction;
	bool old_info1_layout;
	u32 hs400_bad_taps;
	const u8 (*hs400_calib_table)[SDHI_CALIB_TABLE_MAX];
};

struct renesas_sdhi_of_data_with_quirks {
	const struct renesas_sdhi_of_data *of_data;
	const struct renesas_sdhi_quirks *quirks;
};

/* We want both end_flags to be set before we mark DMA as finished */
#define SDHI_DMA_END_FLAG_DMA		0
#define SDHI_DMA_END_FLAG_ACCESS	1

struct renesas_sdhi_dma {
	unsigned long end_flags;
	enum dma_slave_buswidth dma_buswidth;
	dma_filter_fn filter;
	void (*enable)(struct tmio_mmc_host *host, bool enable);
	struct completion dma_dataend;
	struct work_struct dma_complete;
};

struct renesas_sdhi {
	struct clk *clk;
	struct clk *clkh;
	struct clk *clk_cd;
	struct tmio_mmc_data mmc_data;
	struct renesas_sdhi_dma dma_priv;
	const struct renesas_sdhi_quirks *quirks;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default, *pins_uhs;
	void __iomem *scc_ctl;
	u32 scc_tappos;
	u32 scc_tappos_hs400;
	const u8 *adjust_hs400_calib_table;
	bool needs_adjust_hs400;
	bool card_is_sdio;

	/* Tuning values: 1 for success, 0 for failure */
	DECLARE_BITMAP(taps, BITS_PER_LONG);
	/* Sampling data comparison: 1 for match, 0 for mismatch */
	DECLARE_BITMAP(smpcmp, BITS_PER_LONG);
	unsigned int tap_num;
	unsigned int tap_set;

	struct reset_control *rstc;
	struct tmio_mmc_host *host;
	struct regulator_dev *rdev;
};

#define host_to_priv(host) \
	container_of((host)->pdata, struct renesas_sdhi, mmc_data)

int renesas_sdhi_probe(struct platform_device *pdev,
		       const struct tmio_mmc_dma_ops *dma_ops,
		       const struct renesas_sdhi_of_data *of_data,
		       const struct renesas_sdhi_quirks *quirks);
void renesas_sdhi_remove(struct platform_device *pdev);
#endif
