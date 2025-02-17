// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Broadcom
 * Copyright (c) 2014 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

/**
 * DOC: VC4 Falcon HDMI module
 *
 * The HDMI core has a state machine and a PHY.  On BCM2835, most of
 * the unit operates off of the HSM clock from CPRMAN.  It also
 * internally uses the PLLH_PIX clock for the PHY.
 *
 * HDMI infoframes are kept within a small packet ram, where each
 * packet can be individually enabled for including in a frame.
 *
 * HDMI audio is implemented entirely within the HDMI IP block.  A
 * register in the HDMI encoder takes SPDIF frames from the DMA engine
 * and transfers them over an internal MAI (multi-channel audio
 * interconnect) bus to the encoder side for insertion into the video
 * blank regions.
 *
 * The driver's HDMI encoder does not yet support power management.
 * The HDMI encoder's power domain and the HSM/pixel clocks are kept
 * continuously running, and only the HDMI logic and packet ram are
 * powered off/on at disable/enable time.
 *
 * The driver does not yet support CEC control, though the HDMI
 * encoder block has CEC support.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/i2c.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/rational.h>
#include <linux/reset.h>
#include <sound/asoundef.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_drm_eld.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include "media/cec.h"
#include "vc4_drv.h"
#include "vc4_hdmi.h"
#include "vc4_hdmi_regs.h"
#include "vc4_regs.h"

#define VC5_HDMI_HORZA_HFP_SHIFT		16
#define VC5_HDMI_HORZA_HFP_MASK			VC4_MASK(28, 16)
#define VC5_HDMI_HORZA_VPOS			BIT(15)
#define VC5_HDMI_HORZA_HPOS			BIT(14)
#define VC5_HDMI_HORZA_HAP_SHIFT		0
#define VC5_HDMI_HORZA_HAP_MASK			VC4_MASK(13, 0)

#define VC5_HDMI_HORZB_HBP_SHIFT		16
#define VC5_HDMI_HORZB_HBP_MASK			VC4_MASK(26, 16)
#define VC5_HDMI_HORZB_HSP_SHIFT		0
#define VC5_HDMI_HORZB_HSP_MASK			VC4_MASK(10, 0)

#define VC5_HDMI_VERTA_VSP_SHIFT		24
#define VC5_HDMI_VERTA_VSP_MASK			VC4_MASK(28, 24)
#define VC5_HDMI_VERTA_VFP_SHIFT		16
#define VC5_HDMI_VERTA_VFP_MASK			VC4_MASK(22, 16)
#define VC5_HDMI_VERTA_VAL_SHIFT		0
#define VC5_HDMI_VERTA_VAL_MASK			VC4_MASK(12, 0)

#define VC5_HDMI_VERTB_VSPO_SHIFT		16
#define VC5_HDMI_VERTB_VSPO_MASK		VC4_MASK(29, 16)

# define VC4_HD_M_SW_RST			BIT(2)
# define VC4_HD_M_ENABLE			BIT(0)

#define CEC_CLOCK_FREQ 40000
#define VC4_HSM_CLOCK 163682864

#define HDMI_CODEC_CHMAP_IDX_UNKNOWN  -1

/*
 * CEA speaker placement for HDMI 1.4:
 *
 *  FL  FLC   FC   FRC   FR   FRW
 *
 *                                  LFE
 *
 *  RL  RLC   RC   RRC   RR
 *
 *  Speaker placement has to be extended to support HDMI 2.0
 */
enum hdmi_codec_cea_spk_placement {
	FL  = BIT(0),	/* Front Left           */
	FC  = BIT(1),	/* Front Center         */
	FR  = BIT(2),	/* Front Right          */
	FLC = BIT(3),	/* Front Left Center    */
	FRC = BIT(4),	/* Front Right Center   */
	RL  = BIT(5),	/* Rear Left            */
	RC  = BIT(6),	/* Rear Center          */
	RR  = BIT(7),	/* Rear Right           */
	RLC = BIT(8),	/* Rear Left Center     */
	RRC = BIT(9),	/* Rear Right Center    */
	LFE = BIT(10),	/* Low Frequency Effect */
};

/*
 * cea Speaker allocation structure
 */
struct hdmi_codec_cea_spk_alloc {
	const int ca_id;
	unsigned int n_ch;
	unsigned long mask;
};

/* Channel maps  stereo HDMI */
static const struct snd_pcm_chmap_elem hdmi_codec_stereo_chmaps[] = {
	{ .channels = 2,
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ }
};

/* Channel maps for multi-channel playbacks, up to 8 n_ch */
static const struct snd_pcm_chmap_elem hdmi_codec_8ch_chmaps[] = {
	{ .channels = 2, /* CA_ID 0x00 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR } },
	{ .channels = 4, /* CA_ID 0x01 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA } },
	{ .channels = 4, /* CA_ID 0x02 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC } },
	{ .channels = 4, /* CA_ID 0x03 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC } },
	{ .channels = 6, /* CA_ID 0x04 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 6, /* CA_ID 0x05 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 6, /* CA_ID 0x06 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 6, /* CA_ID 0x07 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 6, /* CA_ID 0x08 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 6, /* CA_ID 0x09 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 6, /* CA_ID 0x0A */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 6, /* CA_ID 0x0B */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR } },
	{ .channels = 8, /* CA_ID 0x0C */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 8, /* CA_ID 0x0D */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 8, /* CA_ID 0x0E */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 8, /* CA_ID 0x0F */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RC, SNDRV_CHMAP_NA } },
	{ .channels = 8, /* CA_ID 0x10 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RLC, SNDRV_CHMAP_RRC } },
	{ .channels = 8, /* CA_ID 0x11 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RLC, SNDRV_CHMAP_RRC } },
	{ .channels = 8, /* CA_ID 0x12 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RLC, SNDRV_CHMAP_RRC } },
	{ .channels = 8, /* CA_ID 0x13 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_RL, SNDRV_CHMAP_RR,
		   SNDRV_CHMAP_RLC, SNDRV_CHMAP_RRC } },
	{ .channels = 8, /* CA_ID 0x14 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x15 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x16 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x17 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x18 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x19 */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1A */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1B */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1C */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1D */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_NA, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1E */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ .channels = 8, /* CA_ID 0x1F */
	  .map = { SNDRV_CHMAP_FL, SNDRV_CHMAP_FR, SNDRV_CHMAP_LFE,
		   SNDRV_CHMAP_FC, SNDRV_CHMAP_NA, SNDRV_CHMAP_NA,
		   SNDRV_CHMAP_FLC, SNDRV_CHMAP_FRC } },
	{ }
};

/*
 * hdmi_codec_channel_alloc: speaker configuration available for CEA
 *
 * This is an ordered list that must match with hdmi_codec_8ch_chmaps struct
 * The preceding ones have better chances to be selected by
 * hdmi_codec_get_ch_alloc_table_idx().
 */
static const struct hdmi_codec_cea_spk_alloc hdmi_codec_channel_alloc[] = {
	{ .ca_id = 0x00, .n_ch = 2,
	  .mask = FL | FR},
	/* 2.1 */
	{ .ca_id = 0x01, .n_ch = 4,
	  .mask = FL | FR | LFE},
	/* Dolby Surround */
	{ .ca_id = 0x02, .n_ch = 4,
	  .mask = FL | FR | FC },
	/* surround51 */
	{ .ca_id = 0x0b, .n_ch = 6,
	  .mask = FL | FR | LFE | FC | RL | RR},
	/* surround40 */
	{ .ca_id = 0x08, .n_ch = 6,
	  .mask = FL | FR | RL | RR },
	/* surround41 */
	{ .ca_id = 0x09, .n_ch = 6,
	  .mask = FL | FR | LFE | RL | RR },
	/* surround50 */
	{ .ca_id = 0x0a, .n_ch = 6,
	  .mask = FL | FR | FC | RL | RR },
	/* 6.1 */
	{ .ca_id = 0x0f, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | RC },
	/* surround71 */
	{ .ca_id = 0x13, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | RLC | RRC },
	/* others */
	{ .ca_id = 0x03, .n_ch = 8,
	  .mask = FL | FR | LFE | FC },
	{ .ca_id = 0x04, .n_ch = 8,
	  .mask = FL | FR | RC},
	{ .ca_id = 0x05, .n_ch = 8,
	  .mask = FL | FR | LFE | RC },
	{ .ca_id = 0x06, .n_ch = 8,
	  .mask = FL | FR | FC | RC },
	{ .ca_id = 0x07, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RC },
	{ .ca_id = 0x0c, .n_ch = 8,
	  .mask = FL | FR | RC | RL | RR },
	{ .ca_id = 0x0d, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | RC },
	{ .ca_id = 0x0e, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | RC },
	{ .ca_id = 0x10, .n_ch = 8,
	  .mask = FL | FR | RL | RR | RLC | RRC },
	{ .ca_id = 0x11, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | RLC | RRC },
	{ .ca_id = 0x12, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | RLC | RRC },
	{ .ca_id = 0x14, .n_ch = 8,
	  .mask = FL | FR | FLC | FRC },
	{ .ca_id = 0x15, .n_ch = 8,
	  .mask = FL | FR | LFE | FLC | FRC },
	{ .ca_id = 0x16, .n_ch = 8,
	  .mask = FL | FR | FC | FLC | FRC },
	{ .ca_id = 0x17, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | FLC | FRC },
	{ .ca_id = 0x18, .n_ch = 8,
	  .mask = FL | FR | RC | FLC | FRC },
	{ .ca_id = 0x19, .n_ch = 8,
	  .mask = FL | FR | LFE | RC | FLC | FRC },
	{ .ca_id = 0x1a, .n_ch = 8,
	  .mask = FL | FR | RC | FC | FLC | FRC },
	{ .ca_id = 0x1b, .n_ch = 8,
	  .mask = FL | FR | LFE | RC | FC | FLC | FRC },
	{ .ca_id = 0x1c, .n_ch = 8,
	  .mask = FL | FR | RL | RR | FLC | FRC },
	{ .ca_id = 0x1d, .n_ch = 8,
	  .mask = FL | FR | LFE | RL | RR | FLC | FRC },
	{ .ca_id = 0x1e, .n_ch = 8,
	  .mask = FL | FR | FC | RL | RR | FLC | FRC },
	{ .ca_id = 0x1f, .n_ch = 8,
	  .mask = FL | FR | LFE | FC | RL | RR | FLC | FRC },
};

static unsigned long hdmi_codec_spk_mask_from_alloc(int spk_alloc)
{
	int i;
	static const unsigned long hdmi_codec_eld_spk_alloc_bits[] = {
		[0] = FL | FR, [1] = LFE, [2] = FC, [3] = RL | RR,
		[4] = RC, [5] = FLC | FRC, [6] = RLC | RRC,
	};
	unsigned long spk_mask = 0;

	for (i = 0; i < ARRAY_SIZE(hdmi_codec_eld_spk_alloc_bits); i++) {
		if (spk_alloc & (1 << i))
			spk_mask |= hdmi_codec_eld_spk_alloc_bits[i];
	}

	return spk_mask;
}

static int hdmi_codec_get_ch_alloc_table_idx(struct vc4_hdmi *vc4_hdmi,
					     unsigned char channels)
{
	struct drm_connector *connector = &vc4_hdmi->connector;
	int i;
	u8 spk_alloc;
	unsigned long spk_mask;
	const struct hdmi_codec_cea_spk_alloc *cap = hdmi_codec_channel_alloc;

	spk_alloc = drm_eld_get_spk_alloc(connector->eld);
	spk_mask = hdmi_codec_spk_mask_from_alloc(spk_alloc);

	for (i = 0; i < ARRAY_SIZE(hdmi_codec_channel_alloc); i++, cap++) {
		/* If spk_alloc == 0, HDMI is unplugged return stereo config*/
		if (!spk_alloc && cap->ca_id == 0)
			return i;
		if (cap->n_ch != channels)
			continue;
		if (!(cap->mask == (spk_mask & cap->mask)))
			continue;
		return i;
	}

	return -EINVAL;
}

static void hdmi_codec_eld_chmap(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_connector *connector = &vc4_hdmi->connector;
	u8 spk_alloc;
	unsigned long spk_mask;

	spk_alloc = drm_eld_get_spk_alloc(connector->eld);
	spk_mask = hdmi_codec_spk_mask_from_alloc(spk_alloc);

	/* Detect if only stereo supported, else return 8 channels mappings */
	if ((spk_mask & ~(FL | FR)))
		vc4_hdmi->audio.chmap = hdmi_codec_8ch_chmaps;
	else
		vc4_hdmi->audio.chmap = hdmi_codec_stereo_chmaps;
}

static int vc4_hdmi_debugfs_regs(struct seq_file *m, void *unused)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct vc4_hdmi *vc4_hdmi = node->info_ent->data;
	struct drm_printer p = drm_seq_file_printer(m);

	drm_print_regset32(&p, &vc4_hdmi->hdmi_regset);
	drm_print_regset32(&p, &vc4_hdmi->hd_regset);

	return 0;
}

static void vc4_hdmi_reset(struct vc4_hdmi *vc4_hdmi)
{
	HDMI_WRITE(HDMI_SW_RESET_CONTROL,
		   VC4_HDMI_SW_RESET_HDMI |
		   VC4_HDMI_SW_RESET_FORMAT_DETECT);

	HDMI_WRITE(HDMI_SW_RESET_CONTROL, 0);
}

static void vc5_hdmi_reset(struct vc4_hdmi *vc4_hdmi)
{
	reset_control_reset(vc4_hdmi->reset);

	HDMI_WRITE(HDMI_DVP_CTL, 0);
}

static enum drm_connector_status
vc4_hdmi_connector_detect(struct drm_connector *connector, bool force)
{
	struct vc4_hdmi *vc4_hdmi = connector_to_vc4_hdmi(connector);
	bool connected = false;

	if (vc4_hdmi->hpd_gpio) {
		if (gpio_get_value_cansleep(vc4_hdmi->hpd_gpio) ^
		    vc4_hdmi->hpd_active_low)
			connected = true;
	} else if (drm_probe_ddc(vc4_hdmi->ddc))
		connected = true;
	if (HDMI_READ(HDMI_HOTPLUG) & VC4_HDMI_HOTPLUG_CONNECTED)
		connected = true;
	if (connected) {
		if (connector->status != connector_status_connected) {
			struct edid *edid = drm_get_edid(connector, vc4_hdmi->ddc);

			if (edid) {
				cec_s_phys_addr_from_edid(vc4_hdmi->cec_adap, edid);
				vc4_hdmi->encoder.hdmi_monitor = drm_detect_hdmi_monitor(edid);
				drm_connector_update_edid_property(connector, edid);
				kfree(edid);
			}
		}
		return connector_status_connected;
	}
	cec_phys_addr_invalidate(vc4_hdmi->cec_adap);
	return connector_status_disconnected;
}

static void vc4_hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static int vc4_hdmi_connector_get_modes(struct drm_connector *connector)
{
	struct vc4_hdmi *vc4_hdmi = connector_to_vc4_hdmi(connector);
	struct vc4_hdmi_encoder *vc4_encoder = &vc4_hdmi->encoder;
	int ret = 0;
	struct edid *edid;

	edid = drm_get_edid(connector, vc4_hdmi->ddc);
	cec_s_phys_addr_from_edid(vc4_hdmi->cec_adap, edid);
	if (!edid)
		return -ENODEV;

	vc4_encoder->hdmi_monitor = drm_detect_hdmi_monitor(edid);

	drm_connector_update_edid_property(connector, edid);
	ret = drm_add_edid_modes(connector, edid);
	kfree(edid);

	return ret;
}

static void vc4_hdmi_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
	drm_atomic_helper_connector_tv_reset(connector);
}

static const struct drm_connector_funcs vc4_hdmi_connector_funcs = {
	.detect = vc4_hdmi_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_hdmi_connector_destroy,
	.reset = vc4_hdmi_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs vc4_hdmi_connector_helper_funcs = {
	.get_modes = vc4_hdmi_connector_get_modes,
};

static int vc4_hdmi_connector_init(struct drm_device *dev,
				   struct vc4_hdmi *vc4_hdmi)
{
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base.base;
	int ret;

	drm_connector_init(dev, connector, &vc4_hdmi_connector_funcs,
			   DRM_MODE_CONNECTOR_HDMIA);
	drm_connector_helper_add(connector, &vc4_hdmi_connector_helper_funcs);

	/* Create and attach TV margin props to this connector. */
	ret = drm_mode_create_tv_margin_properties(dev);
	if (ret)
		return ret;

	drm_connector_attach_tv_margin_properties(connector);

	connector->polled = (DRM_CONNECTOR_POLL_CONNECT |
			     DRM_CONNECTOR_POLL_DISCONNECT);

	connector->interlace_allowed = 1;
	connector->doublescan_allowed = 0;

	drm_connector_attach_encoder(connector, encoder);

	return 0;
}

static void vc4_hdmi_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs vc4_hdmi_encoder_funcs = {
	.destroy = vc4_hdmi_encoder_destroy,
};

static int vc4_hdmi_stop_packet(struct drm_encoder *encoder,
				enum hdmi_infoframe_type type)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	u32 packet_id = type - 0x80;

	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
		   HDMI_READ(HDMI_RAM_PACKET_CONFIG) & ~BIT(packet_id));

	return wait_for(!(HDMI_READ(HDMI_RAM_PACKET_STATUS) &
			  BIT(packet_id)), 100);
}

static void vc4_hdmi_write_infoframe(struct drm_encoder *encoder,
				     union hdmi_infoframe *frame)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	u32 packet_id = frame->any.type - 0x80;
	const struct vc4_hdmi_register *ram_packet_start =
		&vc4_hdmi->variant->registers[HDMI_RAM_PACKET_START];
	u32 packet_reg = ram_packet_start->offset + VC4_HDMI_PACKET_STRIDE * packet_id;
	void __iomem *base = __vc4_hdmi_get_field_base(vc4_hdmi,
						       ram_packet_start->reg);
	uint8_t buffer[VC4_HDMI_PACKET_STRIDE];
	ssize_t len, i;
	int ret;

	WARN_ONCE(!(HDMI_READ(HDMI_RAM_PACKET_CONFIG) &
		    VC4_HDMI_RAM_PACKET_ENABLE),
		  "Packet RAM has to be on to store the packet.");

	len = hdmi_infoframe_pack(frame, buffer, sizeof(buffer));
	if (len < 0)
		return;

	ret = vc4_hdmi_stop_packet(encoder, frame->any.type);
	if (ret) {
		DRM_ERROR("Failed to wait for infoframe to go idle: %d\n", ret);
		return;
	}

	for (i = 0; i < len; i += 7) {
		writel(buffer[i + 0] << 0 |
		       buffer[i + 1] << 8 |
		       buffer[i + 2] << 16,
		       base + packet_reg);
		packet_reg += 4;

		writel(buffer[i + 3] << 0 |
		       buffer[i + 4] << 8 |
		       buffer[i + 5] << 16 |
		       buffer[i + 6] << 24,
		       base + packet_reg);
		packet_reg += 4;
	}

	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
		   HDMI_READ(HDMI_RAM_PACKET_CONFIG) | BIT(packet_id));
	ret = wait_for((HDMI_READ(HDMI_RAM_PACKET_STATUS) &
			BIT(packet_id)), 100);
	if (ret)
		DRM_ERROR("Failed to wait for infoframe to start: %d\n", ret);
}

static void vc4_hdmi_set_avi_infoframe(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct vc4_hdmi_encoder *vc4_encoder = to_vc4_hdmi_encoder(encoder);
	struct drm_connector *connector = &vc4_hdmi->connector;
	struct drm_connector_state *cstate = connector->state;
	struct drm_crtc *crtc = encoder->crtc;
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	union hdmi_infoframe frame;
	int ret;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame.avi,
						       connector, mode);
	if (ret < 0) {
		DRM_ERROR("couldn't fill AVI infoframe\n");
		return;
	}

	drm_hdmi_avi_infoframe_quant_range(&frame.avi,
					   connector, mode,
					   vc4_encoder->limited_rgb_range ?
					   HDMI_QUANTIZATION_RANGE_LIMITED :
					   HDMI_QUANTIZATION_RANGE_FULL);

	frame.avi.right_bar = cstate->tv.margins.right;
	frame.avi.left_bar = cstate->tv.margins.left;
	frame.avi.top_bar = cstate->tv.margins.top;
	frame.avi.bottom_bar = cstate->tv.margins.bottom;

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_spd_infoframe(struct drm_encoder *encoder)
{
	union hdmi_infoframe frame;
	int ret;

	ret = hdmi_spd_infoframe_init(&frame.spd, "Broadcom", "Videocore");
	if (ret < 0) {
		DRM_ERROR("couldn't fill SPD infoframe\n");
		return;
	}

	frame.spd.sdi = HDMI_SPD_SDI_PC;

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_audio_infoframe(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	union hdmi_infoframe frame;
	int ret;

	ret = hdmi_audio_infoframe_init(&frame.audio);

	frame.audio.coding_type = HDMI_AUDIO_CODING_TYPE_STREAM;
	frame.audio.sample_frequency = HDMI_AUDIO_SAMPLE_FREQUENCY_STREAM;
	frame.audio.sample_size = HDMI_AUDIO_SAMPLE_SIZE_STREAM;
	frame.audio.channels = vc4_hdmi->audio.channels;

	/* Select a channel allocation that matches with ELD and pcm channels */
	frame.audio.channel_allocation = vc4_hdmi->audio.chmap_idx;

	vc4_hdmi_write_infoframe(encoder, &frame);
}

static void vc4_hdmi_set_infoframes(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);

	vc4_hdmi_set_avi_infoframe(encoder);
	vc4_hdmi_set_spd_infoframe(encoder);
	/*
	 * If audio was streaming, then we need to reenabled the audio
	 * infoframe here during encoder_enable.
	 */
	if (vc4_hdmi->audio.streaming)
		vc4_hdmi_set_audio_infoframe(encoder);
}

static void vc4_hdmi_encoder_disable(struct drm_encoder *encoder)
{
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	int ret;

	HDMI_WRITE(HDMI_RAM_PACKET_CONFIG, 0);

	if (vc4_hdmi->variant->phy_disable)
		vc4_hdmi->variant->phy_disable(vc4_hdmi);

	HDMI_WRITE(HDMI_VID_CTL,
		   HDMI_READ(HDMI_VID_CTL) & ~VC4_HD_VID_CTL_ENABLE);

	clk_disable_unprepare(vc4_hdmi->hsm_clock);
	clk_disable_unprepare(vc4_hdmi->pixel_clock);

	ret = pm_runtime_put(&vc4_hdmi->pdev->dev);
	if (ret < 0)
		DRM_ERROR("Failed to release power domain: %d\n", ret);
}

static void vc4_hdmi_csc_setup(struct vc4_hdmi *vc4_hdmi, bool enable)
{
	u32 csc_ctl;

	csc_ctl = VC4_SET_FIELD(VC4_HD_CSC_CTL_ORDER_BGR,
				VC4_HD_CSC_CTL_ORDER);

	if (enable) {
		/* CEA VICs other than #1 requre limited range RGB
		 * output unless overridden by an AVI infoframe.
		 * Apply a colorspace conversion to squash 0-255 down
		 * to 16-235.  The matrix here is:
		 *
		 * [ 0      0      0.8594 16]
		 * [ 0      0.8594 0      16]
		 * [ 0.8594 0      0      16]
		 * [ 0      0      0       1]
		 */
		csc_ctl |= VC4_HD_CSC_CTL_ENABLE;
		csc_ctl |= VC4_HD_CSC_CTL_RGB2YCC;
		csc_ctl |= VC4_SET_FIELD(VC4_HD_CSC_CTL_MODE_CUSTOM,
					 VC4_HD_CSC_CTL_MODE);

		HDMI_WRITE(HDMI_CSC_12_11, (0x000 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_14_13, (0x100 << 16) | 0x6e0);
		HDMI_WRITE(HDMI_CSC_22_21, (0x6e0 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_24_23, (0x100 << 16) | 0x000);
		HDMI_WRITE(HDMI_CSC_32_31, (0x000 << 16) | 0x6e0);
		HDMI_WRITE(HDMI_CSC_34_33, (0x100 << 16) | 0x000);
	}

	/* The RGB order applies even when CSC is disabled. */
	HDMI_WRITE(HDMI_CSC_CTL, csc_ctl);
}

static void vc5_hdmi_csc_setup(struct vc4_hdmi *vc4_hdmi, bool enable)
{
	u32 csc_ctl;

	csc_ctl = 0x07;	/* RGB_CONVERT_MODE = custom matrix, || USE_RGB_TO_YCBCR */

	if (enable) {
		/* CEA VICs other than #1 requre limited range RGB
		 * output unless overridden by an AVI infoframe.
		 * Apply a colorspace conversion to squash 0-255 down
		 * to 16-235.  The matrix here is:
		 *
		 * [ 0.8594 0      0      16]
		 * [ 0      0.8594 0      16]
		 * [ 0      0      0.8594 16]
		 * [ 0      0      0       1]
		 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
		 */
		HDMI_WRITE(HDMI_CSC_12_11, (0x0000 << 16) | 0x1b80);
		HDMI_WRITE(HDMI_CSC_14_13, (0x0400 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_22_21, (0x1b80 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_24_23, (0x0400 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_32_31, (0x0000 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_34_33, (0x0400 << 16) | 0x1b80);
	} else {
		/* Still use the matrix for full range, but make it unity.
		 * Matrix is signed 2p13 fixed point, with signed 9p6 offsets
		 */
		HDMI_WRITE(HDMI_CSC_12_11, (0x0000 << 16) | 0x2000);
		HDMI_WRITE(HDMI_CSC_14_13, (0x0000 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_22_21, (0x2000 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_24_23, (0x0000 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_32_31, (0x0000 << 16) | 0x0000);
		HDMI_WRITE(HDMI_CSC_34_33, (0x0000 << 16) | 0x2000);
	}

	HDMI_WRITE(HDMI_CSC_CTL, csc_ctl);
}

static void vc4_hdmi_set_timings(struct vc4_hdmi *vc4_hdmi,
				 struct drm_display_mode *mode)
{
	bool hsync_pos = mode->flags & DRM_MODE_FLAG_PHSYNC;
	bool vsync_pos = mode->flags & DRM_MODE_FLAG_PVSYNC;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 pixel_rep = (mode->flags & DRM_MODE_FLAG_DBLCLK) ? 2 : 1;
	u32 verta = (VC4_SET_FIELD(mode->crtc_vsync_end - mode->crtc_vsync_start,
				   VC4_HDMI_VERTA_VSP) |
		     VC4_SET_FIELD(mode->crtc_vsync_start - mode->crtc_vdisplay,
				   VC4_HDMI_VERTA_VFP) |
		     VC4_SET_FIELD(mode->crtc_vdisplay, VC4_HDMI_VERTA_VAL));
	u32 vertb = (VC4_SET_FIELD(0, VC4_HDMI_VERTB_VSPO) |
		     VC4_SET_FIELD(mode->crtc_vtotal - mode->crtc_vsync_end,
				   VC4_HDMI_VERTB_VBP));
	u32 vertb_even = (VC4_SET_FIELD(0, VC4_HDMI_VERTB_VSPO) |
			  VC4_SET_FIELD(mode->crtc_vtotal -
					mode->crtc_vsync_end -
					interlaced,
					VC4_HDMI_VERTB_VBP));

	HDMI_WRITE(HDMI_HORZA,
		   (vsync_pos ? VC4_HDMI_HORZA_VPOS : 0) |
		   (hsync_pos ? VC4_HDMI_HORZA_HPOS : 0) |
		   VC4_SET_FIELD(mode->hdisplay * pixel_rep,
				 VC4_HDMI_HORZA_HAP));

	HDMI_WRITE(HDMI_HORZB,
		   VC4_SET_FIELD((mode->htotal -
				  mode->hsync_end) * pixel_rep,
				 VC4_HDMI_HORZB_HBP) |
		   VC4_SET_FIELD((mode->hsync_end -
				  mode->hsync_start) * pixel_rep,
				 VC4_HDMI_HORZB_HSP) |
		   VC4_SET_FIELD((mode->hsync_start -
				  mode->hdisplay) * pixel_rep,
				 VC4_HDMI_HORZB_HFP));

	HDMI_WRITE(HDMI_VERTA0, verta);
	HDMI_WRITE(HDMI_VERTA1, verta);

	HDMI_WRITE(HDMI_VERTB0, vertb_even);
	HDMI_WRITE(HDMI_VERTB1, vertb);

	HDMI_WRITE(HDMI_VID_CTL,
		 (vsync_pos ? 0 : VC4_HD_VID_CTL_VSYNC_LOW) |
		 (hsync_pos ? 0 : VC4_HD_VID_CTL_HSYNC_LOW));
}

static void vc5_hdmi_set_timings(struct vc4_hdmi *vc4_hdmi,
				 struct drm_display_mode *mode)
{
	bool hsync_pos = mode->flags & DRM_MODE_FLAG_PHSYNC;
	bool vsync_pos = mode->flags & DRM_MODE_FLAG_PVSYNC;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	u32 pixel_rep = (mode->flags & DRM_MODE_FLAG_DBLCLK) ? 2 : 1;
	u32 verta = (VC4_SET_FIELD(mode->crtc_vsync_end - mode->crtc_vsync_start,
				   VC5_HDMI_VERTA_VSP) |
		     VC4_SET_FIELD(mode->crtc_vsync_start - mode->crtc_vdisplay,
				   VC5_HDMI_VERTA_VFP) |
		     VC4_SET_FIELD(mode->crtc_vdisplay, VC5_HDMI_VERTA_VAL));
	u32 vertb = (VC4_SET_FIELD(0, VC5_HDMI_VERTB_VSPO) |
		     VC4_SET_FIELD(mode->crtc_vtotal - mode->crtc_vsync_end,
				   VC4_HDMI_VERTB_VBP));
	u32 vertb_even = (VC4_SET_FIELD(0, VC5_HDMI_VERTB_VSPO) |
			  VC4_SET_FIELD(mode->crtc_vtotal -
					mode->crtc_vsync_end -
					interlaced,
					VC4_HDMI_VERTB_VBP));

	HDMI_WRITE(HDMI_VEC_INTERFACE_XBAR, 0x354021);
	HDMI_WRITE(HDMI_HORZA,
		   (vsync_pos ? VC5_HDMI_HORZA_VPOS : 0) |
		   (hsync_pos ? VC5_HDMI_HORZA_HPOS : 0) |
		   VC4_SET_FIELD(mode->hdisplay * pixel_rep,
				 VC5_HDMI_HORZA_HAP) |
		   VC4_SET_FIELD((mode->hsync_start -
				  mode->hdisplay) * pixel_rep,
				 VC5_HDMI_HORZA_HFP));

	HDMI_WRITE(HDMI_HORZB,
		   VC4_SET_FIELD((mode->htotal -
				  mode->hsync_end) * pixel_rep,
				 VC5_HDMI_HORZB_HBP) |
		   VC4_SET_FIELD((mode->hsync_end -
				  mode->hsync_start) * pixel_rep,
				 VC5_HDMI_HORZB_HSP));

	HDMI_WRITE(HDMI_VERTA0, verta);
	HDMI_WRITE(HDMI_VERTA1, verta);

	HDMI_WRITE(HDMI_VERTB0, vertb_even);
	HDMI_WRITE(HDMI_VERTB1, vertb);

	HDMI_WRITE(HDMI_VID_CTL,
		 (vsync_pos ? 0 : VC4_HD_VID_CTL_VSYNC_LOW) |
		 (hsync_pos ? 0 : VC4_HD_VID_CTL_HSYNC_LOW));

	HDMI_WRITE(HDMI_CLOCK_STOP, 0);
}

static void vc4_hdmi_encoder_enable(struct drm_encoder *encoder)
{
	struct drm_display_mode *mode = &encoder->crtc->state->adjusted_mode;
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);
	struct vc4_hdmi_encoder *vc4_encoder = to_vc4_hdmi_encoder(encoder);
	bool debug_dump_regs = false;
	unsigned long pixel_rate, hsm_rate;
	int ret;

	ret = pm_runtime_get_sync(&vc4_hdmi->pdev->dev);
	if (ret < 0) {
		DRM_ERROR("Failed to retain power domain: %d\n", ret);
		return;
	}

	pixel_rate = mode->clock * 1000 * ((mode->flags & DRM_MODE_FLAG_DBLCLK) ? 2 : 1);
	ret = clk_set_rate(vc4_hdmi->pixel_clock, pixel_rate);
	if (ret) {
		DRM_ERROR("Failed to set pixel clock rate: %d\n", ret);
		return;
	}

	ret = clk_prepare_enable(vc4_hdmi->pixel_clock);
	if (ret) {
		DRM_ERROR("Failed to turn on pixel clock: %d\n", ret);
		return;
	}

	hsm_rate = vc4_hdmi->variant->calc_hsm_clock(vc4_hdmi, pixel_rate);
	ret = clk_set_rate(vc4_hdmi->hsm_clock, hsm_rate);
	if (ret) {
		DRM_ERROR("Failed to set HSM clock rate: %d\n", ret);
		return;
	}

	ret = clk_prepare_enable(vc4_hdmi->hsm_clock);
	if (ret) {
		DRM_ERROR("Failed to turn on HSM clock: %d\n", ret);
		clk_disable_unprepare(vc4_hdmi->pixel_clock);
		return;
	}

	if (vc4_hdmi->variant->reset)
		vc4_hdmi->variant->reset(vc4_hdmi);

	if (vc4_hdmi->variant->phy_init)
		vc4_hdmi->variant->phy_init(vc4_hdmi, mode);

	if (debug_dump_regs) {
		struct drm_printer p = drm_info_printer(&vc4_hdmi->pdev->dev);

		dev_info(&vc4_hdmi->pdev->dev, "HDMI regs before:\n");
		drm_print_regset32(&p, &vc4_hdmi->hdmi_regset);
		drm_print_regset32(&p, &vc4_hdmi->hd_regset);
	}

	HDMI_WRITE(HDMI_VID_CTL, 0);

	HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
		   HDMI_READ(HDMI_SCHEDULER_CONTROL) |
		   VC4_HDMI_SCHEDULER_CONTROL_MANUAL_FORMAT |
		   VC4_HDMI_SCHEDULER_CONTROL_IGNORE_VSYNC_PREDICTS);

	if (vc4_hdmi->variant->set_timings)
		vc4_hdmi->variant->set_timings(vc4_hdmi, mode);

	if (vc4_encoder->hdmi_monitor &&
	    drm_default_rgb_quant_range(mode) == HDMI_QUANTIZATION_RANGE_LIMITED) {
		if (vc4_hdmi->variant->csc_setup)
			vc4_hdmi->variant->csc_setup(vc4_hdmi, true);

		vc4_encoder->limited_rgb_range = true;
	} else {
		if (vc4_hdmi->variant->csc_setup)
			vc4_hdmi->variant->csc_setup(vc4_hdmi, false);

		vc4_encoder->limited_rgb_range = false;
	}

	HDMI_WRITE(HDMI_FIFO_CTL, VC4_HDMI_FIFO_CTL_MASTER_SLAVE_N);

	if (debug_dump_regs) {
		struct drm_printer p = drm_info_printer(&vc4_hdmi->pdev->dev);

		dev_info(&vc4_hdmi->pdev->dev, "HDMI regs after:\n");
		drm_print_regset32(&p, &vc4_hdmi->hdmi_regset);
		drm_print_regset32(&p, &vc4_hdmi->hd_regset);
	}

	HDMI_WRITE(HDMI_VID_CTL,
		   HDMI_READ(HDMI_VID_CTL) |
		   VC4_HD_VID_CTL_ENABLE |
		   VC4_HD_VID_CTL_UNDERFLOW_ENABLE |
		   VC4_HD_VID_CTL_FRAME_COUNTER_RESET);

	if (vc4_encoder->hdmi_monitor) {
		HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(HDMI_SCHEDULER_CONTROL) |
			   VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		ret = wait_for(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			       VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE, 1000);
		WARN_ONCE(ret, "Timeout waiting for "
			  "VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE\n");
	} else {
		HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
			   HDMI_READ(HDMI_RAM_PACKET_CONFIG) &
			   ~(VC4_HDMI_RAM_PACKET_ENABLE));
		HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			   ~VC4_HDMI_SCHEDULER_CONTROL_MODE_HDMI);

		ret = wait_for(!(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
				 VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE), 1000);
		WARN_ONCE(ret, "Timeout waiting for "
			  "!VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE\n");
	}

	if (vc4_encoder->hdmi_monitor) {
		u32 drift;

		WARN_ON(!(HDMI_READ(HDMI_SCHEDULER_CONTROL) &
			  VC4_HDMI_SCHEDULER_CONTROL_HDMI_ACTIVE));
		HDMI_WRITE(HDMI_SCHEDULER_CONTROL,
			   HDMI_READ(HDMI_SCHEDULER_CONTROL) |
			   VC4_HDMI_SCHEDULER_CONTROL_VERT_ALWAYS_KEEPOUT);

		HDMI_WRITE(HDMI_RAM_PACKET_CONFIG,
			   VC4_HDMI_RAM_PACKET_ENABLE);

		vc4_hdmi_set_infoframes(encoder);

		drift = HDMI_READ(HDMI_FIFO_CTL);
		drift &= VC4_HDMI_FIFO_VALID_WRITE_MASK;

		HDMI_WRITE(HDMI_FIFO_CTL,
			   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
		HDMI_WRITE(HDMI_FIFO_CTL,
			   drift | VC4_HDMI_FIFO_CTL_RECENTER);
		usleep_range(1000, 1100);
		HDMI_WRITE(HDMI_FIFO_CTL,
			   drift & ~VC4_HDMI_FIFO_CTL_RECENTER);
		HDMI_WRITE(HDMI_FIFO_CTL,
			   drift | VC4_HDMI_FIFO_CTL_RECENTER);

		ret = wait_for(HDMI_READ(HDMI_FIFO_CTL) &
			       VC4_HDMI_FIFO_CTL_RECENTER_DONE, 1);
		WARN_ONCE(ret, "Timeout waiting for "
			  "VC4_HDMI_FIFO_CTL_RECENTER_DONE");
	}
}

static enum drm_mode_status
vc4_hdmi_encoder_mode_valid(struct drm_encoder *encoder,
			    const struct drm_display_mode *mode)
{
	/*
	 * As stated in RPi's vc4 firmware "HDMI state machine (HSM) clock must
	 * be faster than pixel clock, infinitesimally faster, tested in
	 * simulation. Otherwise, exact value is unimportant for HDMI
	 * operation." This conflicts with bcm2835's vc4 documentation, which
	 * states HSM's clock has to be at least 108% of the pixel clock.
	 *
	 * Real life tests reveal that vc4's firmware statement holds up, and
	 * users are able to use pixel clocks closer to HSM's, namely for
	 * 1920x1200@60Hz. So it was decided to have leave a 1% margin between
	 * both clocks. Which, for RPi0-3 implies a maximum pixel clock of
	 * 162MHz.
	 *
	 * Additionally, the AXI clock needs to be at least 25% of
	 * pixel clock, but HSM ends up being the limiting factor.
	 */
	struct vc4_hdmi *vc4_hdmi = encoder_to_vc4_hdmi(encoder);

	if ((mode->clock * 1000) > vc4_hdmi->variant->max_pixel_clock)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct drm_encoder_helper_funcs vc4_hdmi_encoder_helper_funcs = {
	.mode_valid = vc4_hdmi_encoder_mode_valid,
	.disable = vc4_hdmi_encoder_disable,
	.enable = vc4_hdmi_encoder_enable,
};

static u32 vc4_hdmi_get_hsm_clock(struct vc4_hdmi *vc4_hdmi)
{
	return clk_get_rate(vc4_hdmi->hsm_clock);
}

static u32 vc5_hdmi_get_hsm_clock(struct vc4_hdmi *vc4_hdmi)
{
	return 108000000;
}

static u32 vc4_hdmi_calc_hsm_clock(struct vc4_hdmi *vc4_hdmi, unsigned long pixel_rate)
{
	/*
	 * This is the rate that is set by the firmware.  The number
	 * needs to be a bit higher than the pixel clock rate
	 * (generally 148.5Mhz).
	 */
	return VC4_HSM_CLOCK;
}

static u32 vc5_hdmi_calc_hsm_clock(struct vc4_hdmi *vc4_hdmi, unsigned long pixel_rate)
{
	/*
	 * The HSM rate needs to be slightly greater than the pixel clock, with
	 * a minimum of 108MHz.
	 * Use 101% as this is what the firmware uses.
	 */

	return max_t(unsigned long, 108000000, (pixel_rate / 100) * 101);
}

static u32 vc4_hdmi_channel_map(struct vc4_hdmi *vc4_hdmi, u32 channel_mask)
{
	int i;
	u32 channel_map = 0;

	for (i = 0; i < 8; i++) {
		if (channel_mask & BIT(i))
			channel_map |= i << (3 * i);
	}
	return channel_map;
}

static u32 vc5_hdmi_channel_map(struct vc4_hdmi *vc4_hdmi, u32 channel_mask)
{
	int i;
	u32 channel_map = 0;

	for (i = 0; i < 8; i++) {
		if (channel_mask & BIT(i))
			channel_map |= i << (4 * i);
	}
	return channel_map;
}

/* HDMI audio codec callbacks */
static void vc4_hdmi_audio_set_mai_clock(struct vc4_hdmi *vc4_hdmi)
{
	u32 hsm_clock = vc4_hdmi->variant->get_hsm_clock(vc4_hdmi);
	unsigned long n, m;

	rational_best_approximation(hsm_clock, vc4_hdmi->audio.samplerate,
				    VC4_HD_MAI_SMP_N_MASK >>
				    VC4_HD_MAI_SMP_N_SHIFT,
				    (VC4_HD_MAI_SMP_M_MASK >>
				     VC4_HD_MAI_SMP_M_SHIFT) + 1,
				    &n, &m);

	HDMI_WRITE(HDMI_MAI_SMP,
		 VC4_SET_FIELD(n, VC4_HD_MAI_SMP_N) |
		 VC4_SET_FIELD(m - 1, VC4_HD_MAI_SMP_M));
}

static void vc4_hdmi_set_n_cts(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base.base;
	struct drm_crtc *crtc = encoder->crtc;
	const struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	u32 samplerate = vc4_hdmi->audio.samplerate;
	u32 n, cts;
	u64 tmp;

	n = 128 * samplerate / 1000;
	tmp = (u64)(mode->clock * 1000) * n;
	do_div(tmp, 128 * samplerate);
	cts = tmp;

	HDMI_WRITE(HDMI_CRP_CFG,
		   VC4_HDMI_CRP_CFG_EXTERNAL_CTS_EN |
		   VC4_SET_FIELD(n, VC4_HDMI_CRP_CFG_N));

	/*
	 * We could get slightly more accurate clocks in some cases by
	 * providing a CTS_1 value.  The two CTS values are alternated
	 * between based on the period fields
	 */
	HDMI_WRITE(HDMI_CTS_0, cts);
	HDMI_WRITE(HDMI_CTS_1, cts);
}

static inline struct vc4_hdmi *dai_to_hdmi(struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = snd_soc_dai_get_drvdata(dai);

	return snd_soc_card_get_drvdata(card);
}

static int vc4_hdmi_audio_startup(struct snd_pcm_substream *substream,
				  struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base.base;
	struct drm_connector *connector = &vc4_hdmi->connector;
	int ret;

	if (vc4_hdmi->audio.substream && vc4_hdmi->audio.substream != substream)
		return -EINVAL;

	vc4_hdmi->audio.substream = substream;

	/*
	 * If the HDMI encoder hasn't probed, or the encoder is
	 * currently in DVI mode, treat the codec dai as missing.
	 */
	if (!encoder->crtc || !(HDMI_READ(HDMI_RAM_PACKET_CONFIG) &
				VC4_HDMI_RAM_PACKET_ENABLE))
		return -ENODEV;

	ret = snd_pcm_hw_constraint_eld(substream->runtime, connector->eld);
	if (ret)
		return ret;

	/* Select chmap supported */
	vc4_hdmi->audio.max_channels = 8;
	hdmi_codec_eld_chmap(vc4_hdmi);

	return 0;
}

static int vc4_hdmi_audio_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	return 0;
}

static void vc4_hdmi_audio_reset(struct vc4_hdmi *vc4_hdmi)
{
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base.base;
	struct device *dev = &vc4_hdmi->pdev->dev;
	int ret;

	vc4_hdmi->audio.streaming = false;
	ret = vc4_hdmi_stop_packet(encoder, HDMI_INFOFRAME_TYPE_AUDIO);
	if (ret)
		dev_err(dev, "Failed to stop audio infoframe: %d\n", ret);

	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_RESET);
	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_ERRORF);
	HDMI_WRITE(HDMI_MAI_CTL, VC4_HD_MAI_CTL_FLUSH);
}

static void vc4_hdmi_audio_shutdown(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);

	if (substream != vc4_hdmi->audio.substream)
		return;

	vc4_hdmi_audio_reset(vc4_hdmi);

	vc4_hdmi->audio.substream = NULL;
}

static int sample_rate_to_mai_fmt(int samplerate)
{
   switch(samplerate)
   {
      case 8000:
         return VC4_HDMI_MAI_SAMPLE_RATE_8000;
      case 11025:
         return VC4_HDMI_MAI_SAMPLE_RATE_11025;
      case 12000:
         return VC4_HDMI_MAI_SAMPLE_RATE_12000;
      case 16000:
         return VC4_HDMI_MAI_SAMPLE_RATE_16000;
      case 22050:
         return VC4_HDMI_MAI_SAMPLE_RATE_22050;
      case 24000:
         return VC4_HDMI_MAI_SAMPLE_RATE_24000;
      case 32000:
         return VC4_HDMI_MAI_SAMPLE_RATE_32000;
      case 44100:
         return VC4_HDMI_MAI_SAMPLE_RATE_44100;
      case 48000:
         return VC4_HDMI_MAI_SAMPLE_RATE_48000;
      case 64000:
         return VC4_HDMI_MAI_SAMPLE_RATE_64000;
      case 88200:
         return VC4_HDMI_MAI_SAMPLE_RATE_88200;
      case 96000:
         return VC4_HDMI_MAI_SAMPLE_RATE_96000;
      case 128000:
         return VC4_HDMI_MAI_SAMPLE_RATE_128000;
      case 176400:
         return VC4_HDMI_MAI_SAMPLE_RATE_176400;
      case 192000:
         return VC4_HDMI_MAI_SAMPLE_RATE_192000;
      default:
         return VC4_HDMI_MAI_SAMPLE_RATE_NOT_INDICATED;
   }
}

/* HDMI audio codec callbacks */
static int vc4_hdmi_audio_prepare(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);
	struct device *dev = &vc4_hdmi->pdev->dev;
	u32 audio_packet_config, channel_mask;
	u32 channel_map;
	u32 mai_audio_format;
	u32 mai_sample_rate;
	int idx;

	if (substream != vc4_hdmi->audio.substream)
		return -EINVAL;

	dev_dbg(dev, "%s: %u Hz, %d bit, %d channels AES0=%02x\n",
		__func__,
		substream->runtime->rate,
		snd_pcm_format_width(substream->runtime->format),
		substream->runtime->channels,
		vc4_hdmi->audio.iec_status[0]);

	vc4_hdmi->audio.channels = substream->runtime->channels;
	vc4_hdmi->audio.samplerate = substream->runtime->rate;

	HDMI_WRITE(HDMI_MAI_CTL,
		 VC4_HD_MAI_CTL_RESET |
		 VC4_HD_MAI_CTL_FLUSH |
		 VC4_HD_MAI_CTL_DLATE |
		 VC4_HD_MAI_CTL_ERRORE |
		 VC4_HD_MAI_CTL_ERRORF);

	vc4_hdmi_audio_set_mai_clock(vc4_hdmi);

	mai_sample_rate = sample_rate_to_mai_fmt(vc4_hdmi->audio.samplerate);
	if (vc4_hdmi->audio.iec_status[0] & IEC958_AES0_NONAUDIO &&
		vc4_hdmi->audio.channels == 8)
		mai_audio_format = VC4_HDMI_MAI_FORMAT_HBR;
	else
		mai_audio_format = VC4_HDMI_MAI_FORMAT_PCM;
	HDMI_WRITE(HDMI_MAI_FMT,
		VC4_SET_FIELD(mai_sample_rate, VC4_HDMI_MAI_FORMAT_SAMPLE_RATE) |
		VC4_SET_FIELD(mai_audio_format, VC4_HDMI_MAI_FORMAT_AUDIO_FORMAT));

	/* The B frame identifier should match the value used by alsa-lib (8) */
	audio_packet_config =
		VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_SAMPLE_FLAT |
		VC4_HDMI_AUDIO_PACKET_ZERO_DATA_ON_INACTIVE_CHANNELS |
		VC4_SET_FIELD(0x8, VC4_HDMI_AUDIO_PACKET_B_FRAME_IDENTIFIER);

	channel_mask = GENMASK(vc4_hdmi->audio.channels - 1, 0);
	audio_packet_config |= VC4_SET_FIELD(channel_mask,
					     VC4_HDMI_AUDIO_PACKET_CEA_MASK);

	/* Set the MAI threshold */
	HDMI_WRITE(HDMI_MAI_THR,
		 VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_PANICHIGH) |
		 VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_PANICLOW) |
		 VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_DREQHIGH) |
		 VC4_SET_FIELD(0x10, VC4_HD_MAI_THR_DREQLOW));

	HDMI_WRITE(HDMI_MAI_CONFIG,
		   VC4_HDMI_MAI_CONFIG_BIT_REVERSE |
		   VC4_HDMI_MAI_CONFIG_FORMAT_REVERSE |
		   VC4_SET_FIELD(channel_mask, VC4_HDMI_MAI_CHANNEL_MASK));

	channel_map = vc4_hdmi->variant->channel_map(vc4_hdmi, channel_mask);
	HDMI_WRITE(HDMI_MAI_CHANNEL_MAP, channel_map);
	HDMI_WRITE(HDMI_AUDIO_PACKET_CONFIG, audio_packet_config);
	vc4_hdmi_set_n_cts(vc4_hdmi);

	idx = hdmi_codec_get_ch_alloc_table_idx(vc4_hdmi, vc4_hdmi->audio.channels);
	if (idx < 0) {
		DRM_ERROR("Not able to map channels to speakers (%d)\n", idx);
		vc4_hdmi->audio.chmap_idx = HDMI_CODEC_CHMAP_IDX_UNKNOWN;
	} else {
		vc4_hdmi->audio.chmap_idx = hdmi_codec_channel_alloc[idx].ca_id;
	}

	return 0;
}

static int vc4_hdmi_audio_trigger(struct snd_pcm_substream *substream, int cmd,
				  struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);
	struct drm_encoder *encoder = &vc4_hdmi->encoder.base.base;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		vc4_hdmi_set_audio_infoframe(encoder);
		vc4_hdmi->audio.streaming = true;

		if (vc4_hdmi->variant->phy_rng_enable)
			vc4_hdmi->variant->phy_rng_enable(vc4_hdmi);

		HDMI_WRITE(HDMI_MAI_CTL,
			 VC4_SET_FIELD(vc4_hdmi->audio.channels,
				       VC4_HD_MAI_CTL_CHNUM) |
				       VC4_HD_MAI_CTL_WHOLSMP |
				       VC4_HD_MAI_CTL_CHALIGN |
			 VC4_HD_MAI_CTL_ENABLE);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		HDMI_WRITE(HDMI_MAI_CTL,
			 VC4_HD_MAI_CTL_DLATE |
			 VC4_HD_MAI_CTL_ERRORE |
			 VC4_HD_MAI_CTL_ERRORF);

		if (vc4_hdmi->variant->phy_rng_disable)
			vc4_hdmi->variant->phy_rng_disable(vc4_hdmi);

		vc4_hdmi->audio.streaming = false;

		break;
	default:
		break;
	}

	return 0;
}

static inline struct vc4_hdmi *
snd_component_to_hdmi(struct snd_soc_component *component)
{
	struct snd_soc_card *card = snd_soc_component_get_drvdata(component);

	return snd_soc_card_get_drvdata(card);
}

static int vc4_hdmi_audio_eld_ctl_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);
	struct drm_connector *connector = &vc4_hdmi->connector;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_BYTES;
	uinfo->count = sizeof(connector->eld);

	return 0;
}

static int vc4_hdmi_audio_eld_ctl_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);
	struct drm_connector *connector = &vc4_hdmi->connector;

	memcpy(ucontrol->value.bytes.data, connector->eld,
	       sizeof(connector->eld));

	return 0;
}

static int vc4_spdif_info(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_IEC958;
	uinfo->count = 1;
	return 0;
}

static int vc4_spdif_playback_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);

	memcpy(ucontrol->value.iec958.status, vc4_hdmi->audio.iec_status,
		sizeof(vc4_hdmi->audio.iec_status));

	return 0;
}

static int vc4_spdif_playback_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);

	memcpy(vc4_hdmi->audio.iec_status, ucontrol->value.iec958.status,
		sizeof(vc4_hdmi->audio.iec_status));

	return 0;
}

static int vc4_spdif_mask_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	memset(ucontrol->value.iec958.status, 0xff,
		FIELD_SIZEOF(struct vc4_hdmi_audio, iec_status));

	return 0;
}

/*
 * ALSA API channel-map control callbacks
 */
static int vc4_chmap_ctl_info(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = vc4_hdmi->audio.max_channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = SNDRV_CHMAP_LAST;

	return 0;
}

static int vc4_chmap_ctl_get(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);
	unsigned const char *map;
	unsigned int i;

	if (!vc4_hdmi->audio.chmap)
		return -EINVAL;

	map = vc4_hdmi->audio.chmap[vc4_hdmi->audio.chmap_idx].map;

	for (i = 0; i < vc4_hdmi->audio.max_channels; i++) {
		if (vc4_hdmi->audio.chmap_idx == HDMI_CODEC_CHMAP_IDX_UNKNOWN)
			ucontrol->value.integer.value[i] = 0;
		else
			ucontrol->value.integer.value[i] = map[i];
	}
	return 0;
}

static int vc4_chmap_ctl_tlv(struct snd_kcontrol *kcontrol, int op_flag,
			     unsigned int size, unsigned int __user *tlv)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct vc4_hdmi *vc4_hdmi = snd_component_to_hdmi(component);
	const struct snd_pcm_chmap_elem *map;
	unsigned int __user *dst;
	int c, count = 0;

	if (!vc4_hdmi->audio.chmap)
		return -EINVAL;
	if (size < 8)
		return -ENOMEM;
	if (put_user(SNDRV_CTL_TLVT_CONTAINER, tlv))
		return -EFAULT;
	size -= 8;
	dst = tlv + 2;
	for (map = vc4_hdmi->audio.chmap; map->channels; map++) {
		int chs_bytes = map->channels * 4;
		//if (!valid_chmap_channels(info, map->channels))
		//	continue;
		if (size < 8)
			return -ENOMEM;
		if (put_user(SNDRV_CTL_TLVT_CHMAP_FIXED, dst) ||
		    put_user(chs_bytes, dst + 1))
			return -EFAULT;
		dst += 2;
		size -= 8;
		count += 8;
		if (size < chs_bytes)
			return -ENOMEM;
		size -= chs_bytes;
		count += chs_bytes;
		for (c = 0; c < map->channels; c++) {
			if (put_user(map->map[c], dst))
				return -EFAULT;
			dst++;
		}
	}
	if (put_user(count, tlv + 1))
		return -EFAULT;
	return 0;
}

static const struct snd_kcontrol_new vc4_hdmi_audio_controls[] = {
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = "ELD",
		.info = vc4_hdmi_audio_eld_ctl_info,
		.get = vc4_hdmi_audio_eld_ctl_get,
	},
	{
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", PLAYBACK, DEFAULT),
		.info =    vc4_spdif_info,
		.get =     vc4_spdif_playback_get,
		.put =     vc4_spdif_playback_put,
	},
	{
		.iface =   SNDRV_CTL_ELEM_IFACE_MIXER,
		.name =    SNDRV_CTL_NAME_IEC958("", PLAYBACK, MASK),
		.info =    vc4_spdif_info,
		.get =     vc4_spdif_mask_get,
	},
	{
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			SNDRV_CTL_ELEM_ACCESS_TLV_READ |
			SNDRV_CTL_ELEM_ACCESS_TLV_CALLBACK,
		.iface = SNDRV_CTL_ELEM_IFACE_PCM,
		.name = "Playback Channel Map",
		.info = vc4_chmap_ctl_info,
		.get = vc4_chmap_ctl_get,
		.tlv.c = vc4_chmap_ctl_tlv,
	},
};

static const struct snd_soc_dapm_widget vc4_hdmi_audio_widgets[] = {
	SND_SOC_DAPM_OUTPUT("TX"),
};

static const struct snd_soc_dapm_route vc4_hdmi_audio_routes[] = {
	{ "TX", NULL, "Playback" },
};

static const struct snd_soc_component_driver vc4_hdmi_audio_component_drv = {
	.controls		= vc4_hdmi_audio_controls,
	.num_controls		= ARRAY_SIZE(vc4_hdmi_audio_controls),
	.dapm_widgets		= vc4_hdmi_audio_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(vc4_hdmi_audio_widgets),
	.dapm_routes		= vc4_hdmi_audio_routes,
	.num_dapm_routes	= ARRAY_SIZE(vc4_hdmi_audio_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

static const struct snd_soc_dai_ops vc4_hdmi_audio_dai_ops = {
	.startup = vc4_hdmi_audio_startup,
	.shutdown = vc4_hdmi_audio_shutdown,
	.prepare = vc4_hdmi_audio_prepare,
	.set_fmt = vc4_hdmi_audio_set_fmt,
	.trigger = vc4_hdmi_audio_trigger,
};

static struct snd_soc_dai_driver vc4_hdmi_audio_codec_dai_drv = {
	.name = "vc4-hdmi-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE,
	},
};

static const struct snd_soc_component_driver vc4_hdmi_audio_cpu_dai_comp = {
	.name = "vc4-hdmi-cpu-dai-component",
};

static int vc4_hdmi_audio_cpu_dai_probe(struct snd_soc_dai *dai)
{
	struct vc4_hdmi *vc4_hdmi = dai_to_hdmi(dai);

	snd_soc_dai_init_dma_data(dai, &vc4_hdmi->audio.dma_data, NULL);

	return 0;
}

static struct snd_soc_dai_driver vc4_hdmi_audio_cpu_dai_drv = {
	.name = "vc4-hdmi-cpu-dai",
	.probe  = vc4_hdmi_audio_cpu_dai_probe,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 |
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE,
	},
	.ops = &vc4_hdmi_audio_dai_ops,
};

static const struct snd_dmaengine_pcm_config pcm_conf = {
	.chan_names[SNDRV_PCM_STREAM_PLAYBACK] = "audio-rx",
	.prepare_slave_config = snd_dmaengine_pcm_prepare_slave_config,
};

static int vc4_hdmi_audio_init(struct vc4_hdmi *vc4_hdmi)
{
	const struct vc4_hdmi_register *mai_data =
		&vc4_hdmi->variant->registers[HDMI_MAI_DATA];
	struct snd_soc_dai_link *dai_link = &vc4_hdmi->audio.link;
	struct snd_soc_card *card = &vc4_hdmi->audio.card;
	struct device *dev = &vc4_hdmi->pdev->dev;
	const __be32 *addr;
	int index;
	int ret;
	int len;

	if (!vc4_hdmi->variant->audio_available)
		return 0;

	if (!of_find_property(dev->of_node, "dmas", &len) ||
	    len == 0) {
		dev_warn(dev,
			 "'dmas' DT property is missing or empty, no HDMI audio\n");
		return 0;
	}

	if (mai_data->reg != VC4_HD) {
		WARN_ONCE(true, "MAI isn't in the HD block\n");
		return -EINVAL;
	}

	/*
	 * Get the physical address of VC4_HD_MAI_DATA. We need to retrieve
	 * the bus address specified in the DT, because the physical address
	 * (the one returned by platform_get_resource()) is not appropriate
	 * for DMA transfers.
	 * This VC/MMU should probably be exposed to avoid this kind of hacks.
	 */
	index = of_property_match_string(dev->of_node, "reg-names", "hd");
	addr = of_get_address(dev->of_node, index, NULL, NULL);

	vc4_hdmi->audio.dma_data.addr = be32_to_cpup(addr) + mai_data->offset;
	vc4_hdmi->audio.dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	vc4_hdmi->audio.dma_data.maxburst = 2;

	vc4_hdmi->audio.iec_status[0] = IEC958_AES0_CON_NOT_COPYRIGHT;
	vc4_hdmi->audio.iec_status[1] =
		IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER;
	vc4_hdmi->audio.iec_status[3] = IEC958_AES3_CON_FS_48000;

	ret = devm_snd_dmaengine_pcm_register(dev, &pcm_conf, 0);
	if (ret) {
		dev_err(dev, "Could not register PCM component: %d\n", ret);
		return ret;
	}

	ret = devm_snd_soc_register_component(dev, &vc4_hdmi_audio_cpu_dai_comp,
					      &vc4_hdmi_audio_cpu_dai_drv, 1);
	if (ret) {
		dev_err(dev, "Could not register CPU DAI: %d\n", ret);
		return ret;
	}

	/* register component and codec dai */
	ret = devm_snd_soc_register_component(dev, &vc4_hdmi_audio_component_drv,
				     &vc4_hdmi_audio_codec_dai_drv, 1);
	if (ret) {
		dev_err(dev, "Could not register component: %d\n", ret);
		return ret;
	}

	dai_link->cpus		= &vc4_hdmi->audio.cpu;
	dai_link->codecs	= &vc4_hdmi->audio.codec;
	dai_link->platforms	= &vc4_hdmi->audio.platform;

	dai_link->num_cpus	= 1;
	dai_link->num_codecs	= 1;
	dai_link->num_platforms	= 1;

	dai_link->name = "MAI";
	dai_link->stream_name = "MAI PCM";
	dai_link->codecs->dai_name = vc4_hdmi_audio_codec_dai_drv.name;
	dai_link->cpus->dai_name = dev_name(dev);
	dai_link->codecs->name = dev_name(dev);
	dai_link->platforms->name = dev_name(dev);

	card->dai_link = dai_link;
	card->num_links = 1;
	card->name = vc4_hdmi->variant->id ? "vc4-hdmi1" : "vc4-hdmi";
	card->driver_name = "vc4-hdmi";
	card->dev = dev;

	/*
	 * Be careful, snd_soc_register_card() calls dev_set_drvdata() and
	 * stores a pointer to the snd card object in dev->driver_data. This
	 * means we cannot use it for something else. The hdmi back-pointer is
	 * now stored in card->drvdata and should be retrieved with
	 * snd_soc_card_get_drvdata() if needed.
	 */
	snd_soc_card_set_drvdata(card, vc4_hdmi);
	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err(dev, "Could not register sound card: %d\n", ret);

	return ret;

}

#ifdef CONFIG_DRM_VC4_HDMI_CEC
static irqreturn_t vc4_cec_irq_handler_thread(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;

	if (vc4_hdmi->cec_irq_was_rx) {
		if (vc4_hdmi->cec_rx_msg.len)
			cec_received_msg(vc4_hdmi->cec_adap,
					 &vc4_hdmi->cec_rx_msg);
	} else if (vc4_hdmi->cec_tx_ok) {
		cec_transmit_done(vc4_hdmi->cec_adap, CEC_TX_STATUS_OK,
				  0, 0, 0, 0);
	} else {
		/*
		 * This CEC implementation makes 1 retry, so if we
		 * get a NACK, then that means it made 2 attempts.
		 */
		cec_transmit_done(vc4_hdmi->cec_adap, CEC_TX_STATUS_NACK,
				  0, 2, 0, 0);
	}
	return IRQ_HANDLED;
}

static void vc4_cec_read_msg(struct vc4_hdmi *vc4_hdmi, u32 cntrl1)
{
	struct cec_msg *msg = &vc4_hdmi->cec_rx_msg;
	unsigned int i;

	msg->len = 1 + ((cntrl1 & VC4_HDMI_CEC_REC_WRD_CNT_MASK) >>
					VC4_HDMI_CEC_REC_WRD_CNT_SHIFT);

	if (msg->len > 16) {
		DRM_ERROR("Attempting to read too much data (%d)\n", msg->len);
		return;
	}
	for (i = 0; i < msg->len; i += 4) {
		u32 val = HDMI_READ(HDMI_CEC_RX_DATA_1 + (i>>2));

		msg->msg[i] = val & 0xff;
		msg->msg[i + 1] = (val >> 8) & 0xff;
		msg->msg[i + 2] = (val >> 16) & 0xff;
		msg->msg[i + 3] = (val >> 24) & 0xff;
	}
}

static irqreturn_t vc4_cec_irq_handler(int irq, void *priv)
{
	struct vc4_hdmi *vc4_hdmi = priv;
	u32 stat = HDMI_READ(HDMI_CEC_CPU_STATUS);
	u32 cntrl1, cntrl5;

	if (!(stat & vc4_hdmi->variant->cec_mask))
		return IRQ_NONE;
	vc4_hdmi->cec_rx_msg.len = 0;
	cntrl1 = HDMI_READ(HDMI_CEC_CNTRL_1);
	cntrl5 = HDMI_READ(HDMI_CEC_CNTRL_5);
	vc4_hdmi->cec_irq_was_rx = cntrl5 & VC4_HDMI_CEC_RX_CEC_INT;
	if (vc4_hdmi->cec_irq_was_rx) {
		vc4_cec_read_msg(vc4_hdmi, cntrl1);
		cntrl1 |= VC4_HDMI_CEC_CLEAR_RECEIVE_OFF;
		HDMI_WRITE(HDMI_CEC_CNTRL_1, cntrl1);
		cntrl1 &= ~VC4_HDMI_CEC_CLEAR_RECEIVE_OFF;
	} else {
		vc4_hdmi->cec_tx_ok = cntrl1 & VC4_HDMI_CEC_TX_STATUS_GOOD;
		cntrl1 &= ~VC4_HDMI_CEC_START_XMIT_BEGIN;
	}
	HDMI_WRITE(HDMI_CEC_CNTRL_1, cntrl1);
	HDMI_WRITE(HDMI_CEC_CPU_CLEAR, vc4_hdmi->variant->cec_mask);

	return IRQ_WAKE_THREAD;
}

static int vc4_hdmi_cec_adap_enable(struct cec_adapter *adap, bool enable)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	/* clock period in microseconds */
	const u32 usecs = 1000000 / CEC_CLOCK_FREQ;
	u32 val = HDMI_READ(HDMI_CEC_CNTRL_5);

	val &= ~(VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET |
		 VC4_HDMI_CEC_CNT_TO_4700_US_MASK |
		 VC4_HDMI_CEC_CNT_TO_4500_US_MASK);
	val |= ((4700 / usecs) << VC4_HDMI_CEC_CNT_TO_4700_US_SHIFT) |
	       ((4500 / usecs) << VC4_HDMI_CEC_CNT_TO_4500_US_SHIFT);

	if (enable) {
		HDMI_WRITE(HDMI_CEC_CNTRL_5, val |
			   VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET);
		HDMI_WRITE(HDMI_CEC_CNTRL_5, val);
		HDMI_WRITE(HDMI_CEC_CNTRL_2,
			 ((1500 / usecs) << VC4_HDMI_CEC_CNT_TO_1500_US_SHIFT) |
			 ((1300 / usecs) << VC4_HDMI_CEC_CNT_TO_1300_US_SHIFT) |
			 ((800 / usecs) << VC4_HDMI_CEC_CNT_TO_800_US_SHIFT) |
			 ((600 / usecs) << VC4_HDMI_CEC_CNT_TO_600_US_SHIFT) |
			 ((400 / usecs) << VC4_HDMI_CEC_CNT_TO_400_US_SHIFT));
		HDMI_WRITE(HDMI_CEC_CNTRL_3,
			 ((2750 / usecs) << VC4_HDMI_CEC_CNT_TO_2750_US_SHIFT) |
			 ((2400 / usecs) << VC4_HDMI_CEC_CNT_TO_2400_US_SHIFT) |
			 ((2050 / usecs) << VC4_HDMI_CEC_CNT_TO_2050_US_SHIFT) |
			 ((1700 / usecs) << VC4_HDMI_CEC_CNT_TO_1700_US_SHIFT));
		HDMI_WRITE(HDMI_CEC_CNTRL_4,
			 ((4300 / usecs) << VC4_HDMI_CEC_CNT_TO_4300_US_SHIFT) |
			 ((3900 / usecs) << VC4_HDMI_CEC_CNT_TO_3900_US_SHIFT) |
			 ((3600 / usecs) << VC4_HDMI_CEC_CNT_TO_3600_US_SHIFT) |
			 ((3500 / usecs) << VC4_HDMI_CEC_CNT_TO_3500_US_SHIFT));

		HDMI_WRITE(HDMI_CEC_CPU_MASK_CLEAR, vc4_hdmi->variant->cec_mask);
	} else {
		HDMI_WRITE(HDMI_CEC_CPU_MASK_SET, vc4_hdmi->variant->cec_mask);
		HDMI_WRITE(HDMI_CEC_CNTRL_5, val |
			   VC4_HDMI_CEC_TX_SW_RESET | VC4_HDMI_CEC_RX_SW_RESET);
	}
	return 0;
}

static int vc4_hdmi_cec_adap_log_addr(struct cec_adapter *adap, u8 log_addr)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);

	HDMI_WRITE(HDMI_CEC_CNTRL_1,
		   (HDMI_READ(HDMI_CEC_CNTRL_1) & ~VC4_HDMI_CEC_ADDR_MASK) |
		   (log_addr & 0xf) << VC4_HDMI_CEC_ADDR_SHIFT);
	return 0;
}

static int vc4_hdmi_cec_adap_transmit(struct cec_adapter *adap, u8 attempts,
				      u32 signal_free_time, struct cec_msg *msg)
{
	struct vc4_hdmi *vc4_hdmi = cec_get_drvdata(adap);
	u32 val;
	unsigned int i;

	if (msg->len > 16) {
		DRM_ERROR("Attempting to transmit too much data (%d)\n", msg->len);
		return -ENOMEM;
	}
	for (i = 0; i < msg->len; i += 4)
		HDMI_WRITE(HDMI_CEC_TX_DATA_1 + (i>>2),
			   (msg->msg[i]) |
			   (msg->msg[i + 1] << 8) |
			   (msg->msg[i + 2] << 16) |
			   (msg->msg[i + 3] << 24));

	val = HDMI_READ(HDMI_CEC_CNTRL_1);
	val &= ~VC4_HDMI_CEC_START_XMIT_BEGIN;
	HDMI_WRITE(HDMI_CEC_CNTRL_1, val);
	val &= ~VC4_HDMI_CEC_MESSAGE_LENGTH_MASK;
	val |= (msg->len - 1) << VC4_HDMI_CEC_MESSAGE_LENGTH_SHIFT;
	val |= VC4_HDMI_CEC_START_XMIT_BEGIN;

	HDMI_WRITE(HDMI_CEC_CNTRL_1, val);
	return 0;
}

static const struct cec_adap_ops vc4_hdmi_cec_adap_ops = {
	.adap_enable = vc4_hdmi_cec_adap_enable,
	.adap_log_addr = vc4_hdmi_cec_adap_log_addr,
	.adap_transmit = vc4_hdmi_cec_adap_transmit,
};

static int vc4_hdmi_cec_init(struct vc4_hdmi *vc4_hdmi)
{
	struct cec_connector_info conn_info;
	struct platform_device *pdev = vc4_hdmi->pdev;
	u32 value;
	u32 clk_cnt;
	int ret;

	vc4_hdmi->cec_adap = cec_allocate_adapter(&vc4_hdmi_cec_adap_ops,
						  vc4_hdmi, "vc4",
						  CEC_CAP_DEFAULTS |
						  CEC_CAP_CONNECTOR_INFO, 1);
	ret = PTR_ERR_OR_ZERO(vc4_hdmi->cec_adap);
	if (ret < 0)
		return ret;

	cec_fill_conn_info_from_drm(&conn_info, &vc4_hdmi->connector);
	cec_s_conn_info(vc4_hdmi->cec_adap, &conn_info);

	HDMI_WRITE(HDMI_CEC_CPU_MASK_SET, 0xffffffff);
	value = HDMI_READ(HDMI_CEC_CNTRL_1);
	value &= ~VC4_HDMI_CEC_DIV_CLK_CNT_MASK;
	/*
	 * Set the logical address to Unregistered and set the clock
	 * divider: the hsm_clock rate and this divider setting will
	 * give a 40 kHz CEC clock.
	 */
	clk_cnt = vc4_hdmi->variant->cec_input_clock / CEC_CLOCK_FREQ;
	value |= VC4_HDMI_CEC_ADDR_MASK |
		 ((clk_cnt-1) << VC4_HDMI_CEC_DIV_CLK_CNT_SHIFT);
	HDMI_WRITE(HDMI_CEC_CNTRL_1, value);
	ret = devm_request_threaded_irq(&pdev->dev, platform_get_irq(pdev, 0),
					vc4_cec_irq_handler,
					vc4_cec_irq_handler_thread,
					IRQF_SHARED,
					"vc4 hdmi cec", vc4_hdmi);
	if (ret)
		goto err_delete_cec_adap;

	ret = cec_register_adapter(vc4_hdmi->cec_adap, &pdev->dev);
	if (ret < 0)
		goto err_delete_cec_adap;

	return 0;

err_delete_cec_adap:
	cec_delete_adapter(vc4_hdmi->cec_adap);

	return ret;
}

static void vc4_hdmi_cec_exit(struct vc4_hdmi *vc4_hdmi)
{
	cec_unregister_adapter(vc4_hdmi->cec_adap);
}
#else
static int vc4_hdmi_cec_init(struct vc4_hdmi *vc4_hdmi)
{
	return 0;
}

static void vc4_hdmi_cec_exit(struct vc4_hdmi *vc4_hdmi) {};

#endif

static int vc4_hdmi_build_regset(struct vc4_hdmi *vc4_hdmi,
				 struct debugfs_regset32 *regset,
				 enum vc4_hdmi_regs reg)
{
	const struct vc4_hdmi_variant *variant = vc4_hdmi->variant;
	struct debugfs_reg32 *regs;
	unsigned int count = 0;
	unsigned int i;

	regs = kzalloc(variant->num_registers * sizeof(*regs),
		       GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	for (i = 0; i < variant->num_registers; i++) {
		const struct vc4_hdmi_register *field =	&variant->registers[i];

		if (field->reg != reg)
			continue;

		regs[count].name = field->name;
		regs[count].offset = field->offset;
		count++;
	}

	regs = krealloc(regs, count * sizeof(*regs), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	regset->base = __vc4_hdmi_get_field_base(vc4_hdmi, reg);
	regset->regs = regs;
	regset->nregs = count;

	return 0;
}

static int vc4_hdmi_init_resources(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;
	struct device *dev = &pdev->dev;
	int ret;

	vc4_hdmi->hdmicore_regs = vc4_ioremap_regs(pdev, 0);
	if (IS_ERR(vc4_hdmi->hdmicore_regs))
		return PTR_ERR(vc4_hdmi->hdmicore_regs);

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hd_regset, VC4_HD);
	if (ret)
		return ret;

	vc4_hdmi->hd_regs = vc4_ioremap_regs(pdev, 1);
	if (IS_ERR(vc4_hdmi->hd_regs))
		return PTR_ERR(vc4_hdmi->hd_regs);

	ret = vc4_hdmi_build_regset(vc4_hdmi, &vc4_hdmi->hdmi_regset, VC4_HDMI);
	if (ret)
		return ret;

	vc4_hdmi->pixel_clock = devm_clk_get(dev, "pixel");
	if (IS_ERR(vc4_hdmi->pixel_clock)) {
		ret = PTR_ERR(vc4_hdmi->pixel_clock);
		if (ret != -EPROBE_DEFER)
			DRM_ERROR("Failed to get pixel clock\n");
		return ret;
	}

	vc4_hdmi->hsm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_clock);
	}

	return 0;
}

static int vc5_hdmi_init_resources(struct vc4_hdmi *vc4_hdmi)
{
	struct platform_device *pdev = vc4_hdmi->pdev;
	struct device *dev = &pdev->dev;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hdmi");
	if (!res)
		return -ENODEV;

	vc4_hdmi->hdmicore_regs = devm_ioremap(dev, res->start,
					       resource_size(res));
	if (IS_ERR(vc4_hdmi->hdmicore_regs))
		return PTR_ERR(vc4_hdmi->hdmicore_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "hd");
	if (!res)
		return -ENODEV;

	vc4_hdmi->hd_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->hd_regs))
		return PTR_ERR(vc4_hdmi->hd_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cec");
	if (!res)
		return -ENODEV;

	vc4_hdmi->cec_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->cec_regs))
		return PTR_ERR(vc4_hdmi->cec_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "csc");
	if (!res)
		return -ENODEV;

	vc4_hdmi->csc_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->csc_regs))
		return PTR_ERR(vc4_hdmi->csc_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dvp");
	if (!res)
		return -ENODEV;

	vc4_hdmi->dvp_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->dvp_regs))
		return PTR_ERR(vc4_hdmi->dvp_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "intr2");
	if (!res)
		return -ENODEV;

	vc4_hdmi->intr2_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->intr2_regs))
		return PTR_ERR(vc4_hdmi->intr2_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (!res)
		return -ENODEV;

	vc4_hdmi->phy_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->phy_regs))
		return PTR_ERR(vc4_hdmi->phy_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "packet");
	if (!res)
		return -ENODEV;

	vc4_hdmi->ram_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->ram_regs))
		return PTR_ERR(vc4_hdmi->ram_regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rm");
	if (!res)
		return -ENODEV;

	vc4_hdmi->rm_regs = devm_ioremap(dev, res->start, resource_size(res));
	if (IS_ERR(vc4_hdmi->rm_regs))
		return PTR_ERR(vc4_hdmi->rm_regs);

	vc4_hdmi->hsm_clock = devm_clk_get(dev, "hdmi");
	if (IS_ERR(vc4_hdmi->hsm_clock)) {
		DRM_ERROR("Failed to get HDMI state machine clock\n");
		return PTR_ERR(vc4_hdmi->hsm_clock);
	}

	vc4_hdmi->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(vc4_hdmi->reset)) {
		DRM_ERROR("Failed to get HDMI reset line\n");
		return PTR_ERR(vc4_hdmi->reset);
	}

	return 0;
}

static int vc4_hdmi_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	const struct vc4_hdmi_variant *variant;
	struct vc4_hdmi *vc4_hdmi;
	struct drm_encoder *encoder;
	struct device_node *ddc_node;
	u32 value;
	int ret;

	vc4_hdmi = devm_kzalloc(dev, sizeof(*vc4_hdmi), GFP_KERNEL);
	if (!vc4_hdmi)
		return -ENOMEM;
	vc4_hdmi->pdev = pdev;
	variant = of_device_get_match_data(dev);
	vc4_hdmi->variant = variant;
	vc4_hdmi->encoder.base.type = variant->id ? VC4_ENCODER_TYPE_HDMI1 : VC4_ENCODER_TYPE_HDMI0;
	encoder = &vc4_hdmi->encoder.base.base;

	ret = variant->init_resources(vc4_hdmi);
	if (ret)
		return ret;

	ddc_node = of_parse_phandle(dev->of_node, "ddc", 0);
	if (!ddc_node) {
		DRM_ERROR("Failed to find ddc node in device tree\n");
		return -ENODEV;
	}

	vc4_hdmi->ddc = of_find_i2c_adapter_by_node(ddc_node);
	of_node_put(ddc_node);
	if (!vc4_hdmi->ddc) {
		DRM_DEBUG("Failed to get ddc i2c adapter by node\n");
		return -EPROBE_DEFER;
	}

	/* Only use the GPIO HPD pin if present in the DT, otherwise
	 * we'll use the HDMI core's register.
	 */
	if (of_find_property(dev->of_node, "hpd-gpios", &value)) {
		enum of_gpio_flags hpd_gpio_flags;

		vc4_hdmi->hpd_gpio = of_get_named_gpio_flags(dev->of_node,
							 "hpd-gpios", 0,
							 &hpd_gpio_flags);
		if (vc4_hdmi->hpd_gpio < 0) {
			ret = vc4_hdmi->hpd_gpio;
			goto err_unprepare_hsm;
		}

		vc4_hdmi->hpd_active_low = hpd_gpio_flags & OF_GPIO_ACTIVE_LOW;
	}

	/* HDMI core must be enabled. */
	if (!(HDMI_READ(HDMI_M_CTL) & VC4_HD_M_ENABLE)) {
		HDMI_WRITE(HDMI_M_CTL, VC4_HD_M_SW_RST);
		udelay(1);
		HDMI_WRITE(HDMI_M_CTL, 0);

		HDMI_WRITE(HDMI_M_CTL, VC4_HD_M_ENABLE);
	}
	pm_runtime_enable(dev);

	drm_encoder_init(drm, encoder, &vc4_hdmi_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(encoder, &vc4_hdmi_encoder_helper_funcs);

	ret = vc4_hdmi_connector_init(drm, vc4_hdmi);
	if (ret)
		goto err_destroy_encoder;

	ret = vc4_hdmi_cec_init(vc4_hdmi);
	if (ret)
		goto err_destroy_conn;

	ret = vc4_hdmi_audio_init(vc4_hdmi);
	if (ret)
		goto err_free_cec;

	vc4_debugfs_add_file(drm,
			     variant->id ? "hdmi1_regs" : "hdmi_regs",
			     vc4_hdmi_debugfs_regs,
			     vc4_hdmi);

	return 0;

err_free_cec:
	vc4_hdmi_cec_exit(vc4_hdmi);
err_destroy_conn:
	vc4_hdmi_connector_destroy(&vc4_hdmi->connector);
err_destroy_encoder:
	vc4_hdmi_encoder_destroy(encoder);
err_unprepare_hsm:
	pm_runtime_disable(dev);
	put_device(&vc4_hdmi->ddc->dev);

	return ret;
}

static void vc4_hdmi_unbind(struct device *dev, struct device *master,
			    void *data)
{
	/*
	 * snd_soc_register_card will set the device drvdata pointer
	 * to the card being registered.
	 */
	struct snd_soc_card *card = dev_get_drvdata(dev);
	struct vc4_hdmi *vc4_hdmi = snd_soc_card_get_drvdata(card);

	kfree(vc4_hdmi->hdmi_regset.regs);
	kfree(vc4_hdmi->hd_regset.regs);

	vc4_hdmi_cec_exit(vc4_hdmi);
	vc4_hdmi_connector_destroy(&vc4_hdmi->connector);
	vc4_hdmi_encoder_destroy(&vc4_hdmi->encoder.base.base);

	pm_runtime_disable(dev);

	put_device(&vc4_hdmi->ddc->dev);
}

static const struct component_ops vc4_hdmi_ops = {
	.bind   = vc4_hdmi_bind,
	.unbind = vc4_hdmi_unbind,
};

static int vc4_hdmi_dev_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_hdmi_ops);
}

static int vc4_hdmi_dev_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_hdmi_ops);
	return 0;
}

static const struct vc4_hdmi_variant bcm2835_variant = {
	.max_pixel_clock	= 162000000,
	.cec_input_clock	= VC4_HSM_CLOCK,
	.audio_available	= true,
	.registers		= vc4_hdmi_fields,
	.num_registers		= ARRAY_SIZE(vc4_hdmi_fields),

	.init_resources		= vc4_hdmi_init_resources,
	.csc_setup		= vc4_hdmi_csc_setup,
	.reset			= vc4_hdmi_reset,
	.set_timings		= vc4_hdmi_set_timings,
	.phy_init		= vc4_hdmi_phy_init,
	.phy_disable		= vc4_hdmi_phy_disable,
	.phy_rng_enable		= vc4_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc4_hdmi_phy_rng_disable,
	.get_hsm_clock		= vc4_hdmi_get_hsm_clock,
	.calc_hsm_clock		= vc4_hdmi_calc_hsm_clock,
	.channel_map		= vc4_hdmi_channel_map,

	.cec_mask = VC4_HDMI_CPU_CEC,
};

static const struct vc4_hdmi_variant bcm2711_hdmi0_variant = {
	.id			= 0,
	.audio_available	= true,
	.max_pixel_clock	= 297000000,
	.cec_input_clock	= 27000000,
	.registers		= vc5_hdmi_hdmi0_fields,
	.num_registers		= ARRAY_SIZE(vc5_hdmi_hdmi0_fields),
	.phy_lane_mapping	= {
		PHY_LANE_0,
		PHY_LANE_1,
		PHY_LANE_2,
		PHY_LANE_CK,
	},

	.init_resources		= vc5_hdmi_init_resources,
	.csc_setup		= vc5_hdmi_csc_setup,
	.reset			= vc5_hdmi_reset,
	.set_timings		= vc5_hdmi_set_timings,
	.phy_init		= vc5_hdmi_phy_init,
	.phy_rng_enable		= vc5_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc5_hdmi_phy_rng_disable,
	.get_hsm_clock		= vc5_hdmi_get_hsm_clock,
	.calc_hsm_clock		= vc5_hdmi_calc_hsm_clock,
	.channel_map		= vc5_hdmi_channel_map,

	.cec_mask = VC5_HDMI0_CPU_CEC_RX | VC5_HDMI0_CPU_CEC_TX,
};

static const struct vc4_hdmi_variant bcm2711_hdmi1_variant = {
	.id			= 1,
	.audio_available	= true,
	.max_pixel_clock	= 297000000,
	.cec_input_clock	= 27000000,
	.registers		= vc5_hdmi_hdmi1_fields,
	.num_registers		= ARRAY_SIZE(vc5_hdmi_hdmi1_fields),
	.phy_lane_mapping	= {
		PHY_LANE_1,
		PHY_LANE_0,
		PHY_LANE_CK,
		PHY_LANE_2,
	},

	.init_resources		= vc5_hdmi_init_resources,
	.csc_setup		= vc5_hdmi_csc_setup,
	.reset			= vc5_hdmi_reset,
	.set_timings		= vc5_hdmi_set_timings,
	.phy_init		= vc5_hdmi_phy_init,
	.phy_rng_enable		= vc5_hdmi_phy_rng_enable,
	.phy_rng_disable	= vc5_hdmi_phy_rng_disable,
	.get_hsm_clock		= vc5_hdmi_get_hsm_clock,
	.calc_hsm_clock		= vc5_hdmi_calc_hsm_clock,
	.channel_map		= vc5_hdmi_channel_map,

	.cec_mask = VC5_HDMI1_CPU_CEC_RX | VC5_HDMI1_CPU_CEC_TX,
};

static const struct of_device_id vc4_hdmi_dt_match[] = {
	{ .compatible = "brcm,bcm2835-hdmi", .data = &bcm2835_variant },
	{ .compatible = "brcm,bcm2711-hdmi0", .data = &bcm2711_hdmi0_variant },
	{ .compatible = "brcm,bcm2711-hdmi1", .data = &bcm2711_hdmi1_variant },
	{}
};

struct platform_driver vc4_hdmi_driver = {
	.probe = vc4_hdmi_dev_probe,
	.remove = vc4_hdmi_dev_remove,
	.driver = {
		.name = "vc4_hdmi",
		.of_match_table = vc4_hdmi_dt_match,
	},
};
