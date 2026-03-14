/*
 * (C) Copyright 2008-2016 Fuzhou Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <asm/arch/rkplat.h>
#include <asm/unaligned.h>
#include <asm/errno.h>
#include <config.h>
#include <common.h>
#include <errno.h>
#include <fdtdec.h>
#include <fdt_support.h>
#include <linux/list.h>
#include <linux/compat.h>
#include <linux/media-bus-format.h>
#include <malloc.h>
#include <resource.h>
#include <fastboot.h>
#include <../board/rockchip/common/storage/storage.h>

#include "bmp_helper.h"
#include "rockchip_display.h"
#include "rockchip_crtc.h"
#include "rockchip_connector.h"
#include "rockchip_phy.h"
#include "rockchip_panel.h"
#ifndef CONFIG_LCD_CONSOLE_DISABLE
#include <video_font_data.h>
#include <version.h>
#endif

#define DRIVER_VERSION	"develop-v1.0.0"

/***********************************************************************
 *  Rockchip UBOOT DRM driver version
 *
 *  v1.0.0	: add basic version for rockchip drm driver(hjc)
 *
 **********************************************************************/

DECLARE_GLOBAL_DATA_PTR;
static LIST_HEAD(rockchip_display_list);
static LIST_HEAD(logo_cache_list);

#define MEMORY_POOL_SIZE CONFIG_RK_LCD_SIZE
static unsigned long memory_start;
static unsigned long memory_end;
static unsigned long logo_stride_bytes(u32 width, u32 bpp);

#ifdef CONFIG_RK_PWM_BL
extern int rk_pwm_bl_config(int brightness);
#endif
extern uint32 SecureBootEn;
extern uint32 SecureBootLock;

/*
 * the phy types are used by different connectors in public.
 * The current version only has inno hdmi phy for hdmi and tve.
 */
enum public_use_phy {
	NONE,
	INNO_HDMI_PHY
};

/* save public phy data */
struct public_phy_data {
	void *private_date;
	const struct rockchip_phy *phy_drv;
	int phy_node;
	int public_phy_type;
	bool phy_init;
};

/* check which kind of public phy does connector use */
static int check_public_use_phy(struct display_state *state)
{
	int ret = NONE;

#ifdef CONFIG_RKCHIP_INNO_HDMI_PHY
	struct connector_state *conn_state = &state->conn_state;
	int conn_node = conn_state->node;
	const void *blob = state->blob;

	if (!strncmp(fdt_get_name(blob, conn_node, NULL), "tve", 3) ||
	!strncmp(fdt_get_name(blob, conn_node, NULL), "hdmi", 4))
		ret = INNO_HDMI_PHY;
#endif

	return ret;

}

/*
 * get public phy driver and initialize it.
 * The current version only has inno hdmi phy for hdmi and tve.
 */
static int get_public_phy(struct display_state *state,
			  struct public_phy_data *data)
{
	struct connector_state *conn_state = &state->conn_state;
	const void *blob = state->blob;
	const struct rockchip_phy *phy;
	int phy_node;

	switch (data->public_phy_type) {
	case INNO_HDMI_PHY:
#if defined(CONFIG_RKCHIP_RK322XH)
		phy_node = fdt_path_offset(blob, "/hdmiphy");
		if (phy_node < 0) {
			printf("Can't find 322xh hdmiphy node\n");
			return phy_node;
		}
#elif defined(CONFIG_RKCHIP_RK322X)
		phy_node = fdt_path_offset(blob, "/hdmi-phy");
		if (phy_node < 0) {
			printf("Can't find 322x hdmiphy node\n");
			return phy_node;
		}
#endif

		if (!fdt_device_is_available(blob, phy_node))
			return -ENODEV;

		phy = rockchip_get_phy(blob, phy_node);
		if (!phy) {
			printf("failed to find phy driver\n");
			return 0;
		}

		conn_state->phy_node = phy_node;

		if (!phy->funcs || !phy->funcs->init ||
		    phy->funcs->init(state)) {
			printf("failed to init phy driver\n");
			return -EINVAL;
		}
		conn_state->phy = phy;

		printf("inno hdmi phy init success, save it\n");
		data->phy_node = conn_state->phy_node;
		data->private_date = conn_state->phy_private;
		data->phy_drv = conn_state->phy;
		data->phy_init = true;
		return 0;
	default:
		return -EINVAL;
	}
}

static void init_display_buffer(void)
{
	memory_start = gd->fb_base;
	memory_end = memory_start;
}

static int rockchip_calc_fb_size(u32 width, u32 height, u32 bpp,
				 unsigned long *size)
{
	unsigned long stride;

	if (!width || !height || !bpp || !size)
		return -EINVAL;

	stride = logo_stride_bytes(width, bpp);
	if (!stride || height > (~0UL) / stride)
		return -EOVERFLOW;

	*size = stride * height;
	return 0;
}

static void *get_display_buffer(unsigned long size)
{
	unsigned long roundup_memory = roundup(memory_end, PAGE_SIZE);
	void *buf;

	if (!size || size > MEMORY_POOL_SIZE)
		return NULL;
	if (roundup_memory + size > memory_start + MEMORY_POOL_SIZE) {
		printf("failed to alloc %lu byte memory to display\n", size);
		return NULL;
	}
	buf = (void *)roundup_memory;

	memory_end = roundup_memory + size;

	return buf;
}

static unsigned long get_display_size(void)
{
	return memory_end - memory_start;
}

static bool can_direct_logo(int bpp)
{
	return bpp == 24 || bpp == 32;
}

static unsigned long logo_stride_bytes(u32 width, u32 bpp)
{
	return ALIGN((unsigned long)width * bpp, 32) >> 3;
}

static void rockchip_rotate_logo(const struct logo_info *src, struct logo_info *dst,
				 int rotate)
{
	const u8 *src_base = (const u8 *)src->mem + src->offset;
	u8 *dst_base = (u8 *)dst->mem;
	int bytespp = src->bpp >> 3;
	int src_stride = logo_stride_bytes(src->width, src->bpp);
	int dst_stride;
	int x, y, phys_y;

	if ((rotate != 90 && rotate != 270) || !bytespp)
		return;

	dst->width = src->height;
	dst->height = src->width;
	dst->bpp = src->bpp;
	dst->mode = src->mode;
	dst->rotate = rotate;
	dst->ymirror = 0;
	dst->offset = 0;

	dst_stride = logo_stride_bytes(dst->width, dst->bpp);
	if (!dst_base) {
		dst_base = get_display_buffer(dst_stride * dst->height);
		if (!dst_base) {
			printf("failed to alloc rotated logo buffer\n");
			memcpy(dst, src, sizeof(*dst));
			return;
		}
	}

	memset(dst_base, 0, dst_stride * dst->height);

	for (y = 0; y < src->height; y++) {
		phys_y = src->ymirror ? (src->height - 1 - y) : y;
		for (x = 0; x < src->width; x++) {
			int dst_x, dst_y;
			const u8 *pixel = src_base + phys_y * src_stride + x * bytespp;
			u8 *dst_pixel;

			if (rotate == 90) {
				dst_x = src->height - 1 - y;
				dst_y = x;
			} else {
				dst_x = y;
				dst_y = src->width - 1 - x;
			}

			dst_pixel = dst_base + dst_y * dst_stride + dst_x * bytespp;
			memcpy(dst_pixel, pixel, bytespp);
		}
	}

	dst->mem = (char *)dst_base;
}

static void rockchip_prepare_logo(struct logo_info *logo)
{
	struct logo_info rotated;

	if (logo->rotate != 90 && logo->rotate != 270)
		return;

	memset(&rotated, 0, sizeof(rotated));
	rockchip_rotate_logo(logo, &rotated, logo->rotate);
	if (rotated.mem)
		memcpy(logo, &rotated, sizeof(*logo));
}

static void rockchip_fill_argb8888(void *fb, int pixels, u32 color)
{
	u32 *dst = fb;
	int i;

	for (i = 0; i < pixels; i++)
		dst[i] = color;
}

static void rockchip_recolor_logo(struct logo_info *logo)
{
	u8 *base = (u8 *)logo->mem + logo->offset;
	int stride = logo_stride_bytes(logo->width, logo->bpp);
	int x, y;

	if (!base)
		return;

	for (y = 0; y < logo->height; y++) {
		u8 *row = base + y * stride;

		for (x = 0; x < logo->width; x++) {
			u8 *pixel;
			u8 r, g, b;

			if (logo->bpp == 24) {
				pixel = row + x * 3;
				b = pixel[0];
				g = pixel[1];
				r = pixel[2];
				if (r < 32 && g < 32 && b < 32) {
					pixel[0] = 0xFF;
					pixel[1] = 0xFF;
					pixel[2] = 0xFF;
				}
			} else if (logo->bpp == 32) {
				pixel = row + x * 4;
				b = pixel[0];
				g = pixel[1];
				r = pixel[2];
				if (r < 32 && g < 32 && b < 32) {
					pixel[0] = 0xFF;
					pixel[1] = 0xFF;
					pixel[2] = 0xFF;
					pixel[3] = 0xFF;
				}
			}
		}
	}
}

static int get_panel_node(struct display_state *state, int conn_node)
{
	const void *blob = state->blob;
	int panel, ports, port, ep, remote, ph, nodedepth;

	panel = fdt_subnode_offset(blob, conn_node, "panel");
	if (panel > 0)
		return panel;

	ports = fdt_subnode_offset(blob, conn_node, "ports");
	if (ports < 0)
		return -ENODEV;

	fdt_for_each_subnode(blob, port, ports) {
		fdt_for_each_subnode(blob, ep, port) {
			ph = fdt_getprop_u32_default_node(blob, ep, 0,
							  "remote-endpoint", 0);
			if (!ph)
				continue;

			remote = fdt_node_offset_by_phandle(blob, ph);

			nodedepth = fdt_node_depth(blob, remote);
			if (nodedepth < 2)
				continue;

			panel = fdt_supernode_atdepth_offset(blob, remote,
							     nodedepth - 2,
							     NULL);
			break;
		}
	}

	return panel;
}

static int connector_phy_init(struct display_state *state,
			      struct public_phy_data *data)
{
	struct connector_state *conn_state = &state->conn_state;
	int conn_node = conn_state->node;
	const void *blob = state->blob;
	const struct rockchip_phy *phy;
	int phy_node, phandle, type;

	/* does this connector use public phy with others */
	type = check_public_use_phy(state);
	if (type == INNO_HDMI_PHY) {
		/* there is no public phy was initialized */
		if (!data->phy_init) {
			data->public_phy_type = type;
			if(get_public_phy(state, data)) {
				printf("can't find correct public phy type\n");
				free(data);
				return -EINVAL;
			}
			return 0;
		}

		/* if this phy has been initialized, get it directly */
		conn_state->phy_node = data->phy_node;
		conn_state->phy_private = data->private_date;
		conn_state->phy = data->phy_drv;
		return 0;
	}

	/*
	 * if this connector don't use the same phy with others,
	 * just get phy as original method.
	 */
	phandle = fdt_getprop_u32_default_node(blob, conn_node, 0,
					       "phys", -1);
	if (phandle < 0)
		return 0;

	phy_node = fdt_node_offset_by_phandle(blob, phandle);
	if (phy_node < 0) {
		printf("failed to find phy node\n");
		return phy_node;
	}

	phy = rockchip_get_phy(blob, phy_node);
	if (!phy) {
		printf("failed to find phy driver\n");
		return 0;
	}

	conn_state->phy_node = phy_node;

	if (!phy->funcs || !phy->funcs->init ||
	    phy->funcs->init(state)) {
		printf("failed to init phy driver\n");
		return -EINVAL;
	}
	conn_state->phy = phy;

	return 0;
}

static int connector_panel_init(struct display_state *state)
{
	struct connector_state *conn_state = &state->conn_state;
	struct panel_state *panel_state = &state->panel_state;
	const void *blob = state->blob;
	int conn_node = conn_state->node;
	const struct rockchip_panel *panel;
	int panel_node, dsp_lut_node;
	int ret, len;

	panel_node = get_panel_node(state, conn_node);
	if (panel_node < 0) {
		printf("failed to find panel node\n");
		return -ENODEV;
	}

	if (!fdt_device_is_available(blob, panel_node)) {
		printf("panel is disabled\n");
		return -ENODEV;
	}

	panel_state->node = panel_node;

	panel = rockchip_get_panel(blob, panel_node);
	if (!panel) {
		printf("failed to find panel driver\n");
		return 0;
	}

	panel_state->panel = panel;

	ret = rockchip_panel_init(state);
	if (ret) {
		printf("failed to init panel driver\n");
		return ret;
	}

	dsp_lut_node = fdt_subnode_offset(blob, panel_node, "dsp-lut");
	fdt_getprop(blob, dsp_lut_node, "gamma-lut", &len);
	if (len > 0) {
		conn_state->gamma.size  = len / sizeof(u32);
		conn_state->gamma.lut = malloc(len);
		if (!conn_state->gamma.lut) {
			printf("malloc gamma lut failed\n");
			return -ENOMEM;
		}
		if (fdtdec_get_int_array(blob, dsp_lut_node, "gamma-lut",
					 conn_state->gamma.lut,
					 conn_state->gamma.size)) {
			printf("Cannot decode gamma_lut\n");
			conn_state->gamma.lut = NULL;
			return -EINVAL;
		}
		panel_state->dsp_lut_node = dsp_lut_node;
	}

	return 0;
}

int drm_mode_vrefresh(const struct drm_display_mode *mode)
{
	int refresh = 0;
	unsigned int calc_val;

	if (mode->vrefresh > 0) {
		refresh = mode->vrefresh;
	} else if (mode->htotal > 0 && mode->vtotal > 0) {
		int vtotal;

		vtotal = mode->vtotal;
		/* work out vrefresh the value will be x1000 */
		calc_val = (mode->clock * 1000);
		calc_val /= mode->htotal;
		refresh = (calc_val + vtotal / 2) / vtotal;

		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			refresh *= 2;
		if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
			refresh /= 2;
		if (mode->vscan > 1)
			refresh /= mode->vscan;
	}
	return refresh;
}

static int display_get_timing_from_dts(int panel, const void *blob,
				       struct drm_display_mode *mode)
{
	int timing, phandle, native_mode;
	int hactive, vactive, pixelclock;
	int hfront_porch, hback_porch, hsync_len;
	int vfront_porch, vback_porch, vsync_len;
	int val, flags = 0;

	timing = fdt_subnode_offset(blob, panel, "display-timings");
	if (timing < 0)
		return -ENODEV;

	native_mode = fdt_subnode_offset(blob, timing, "timing");
	if (native_mode < 0) {
		phandle = fdt_getprop_u32_default_node(blob, timing, 0,
						       "native-mode", -1);
		native_mode = fdt_node_offset_by_phandle_node(blob, timing, phandle);
		if (native_mode <= 0) {
			printf("failed to get display timings from DT\n");
			return -ENXIO;
		}
	}

#define FDT_GET_INT(val, name) \
	val = fdtdec_get_int(blob, native_mode, name, -1); \
	if (val < 0) { \
		printf("Can't get %s\n", name); \
		return -ENXIO; \
	}

	FDT_GET_INT(hactive, "hactive");
	FDT_GET_INT(vactive, "vactive");
	FDT_GET_INT(pixelclock, "clock-frequency");
	FDT_GET_INT(hsync_len, "hsync-len");
	FDT_GET_INT(hfront_porch, "hfront-porch");
	FDT_GET_INT(hback_porch, "hback-porch");
	FDT_GET_INT(vsync_len, "vsync-len");
	FDT_GET_INT(vfront_porch, "vfront-porch");
	FDT_GET_INT(vback_porch, "vback-porch");
	FDT_GET_INT(val, "hsync-active");
	flags |= val ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	FDT_GET_INT(val, "vsync-active");
	flags |= val ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;

	mode->hdisplay = hactive;
	mode->hsync_start = mode->hdisplay + hfront_porch;
	mode->hsync_end = mode->hsync_start + hsync_len;
	mode->htotal = mode->hsync_end + hback_porch;

	mode->vdisplay = vactive;
	mode->vsync_start = mode->vdisplay + vfront_porch;
	mode->vsync_end = mode->vsync_start + vsync_len;
	mode->vtotal = mode->vsync_end + vback_porch;

	mode->clock = pixelclock / 1000;
	mode->flags = flags;

	return 0;
}

/**
 * drm_mode_set_crtcinfo - set CRTC modesetting timing parameters
 * @p: mode
 * @adjust_flags: a combination of adjustment flags
 *
 * Setup the CRTC modesetting timing parameters for @p, adjusting if necessary.
 *
 * - The CRTC_INTERLACE_HALVE_V flag can be used to halve vertical timings of
 *   interlaced modes.
 * - The CRTC_STEREO_DOUBLE flag can be used to compute the timings for
 *   buffers containing two eyes (only adjust the timings when needed, eg. for
 *   "frame packing" or "side by side full").
 * - The CRTC_NO_DBLSCAN and CRTC_NO_VSCAN flags request that adjustment *not*
 *   be performed for doublescan and vscan > 1 modes respectively.
 */
void drm_mode_set_crtcinfo(struct drm_display_mode *p, int adjust_flags)
{
	if ((p == NULL) || ((p->type & DRM_MODE_TYPE_CRTC_C) == DRM_MODE_TYPE_BUILTIN))
		return;

	if (p->flags & DRM_MODE_FLAG_DBLCLK)
		p->crtc_clock = 2 * p->clock;
	else
		p->crtc_clock = p->clock;
	p->crtc_hdisplay = p->hdisplay;
	p->crtc_hsync_start = p->hsync_start;
	p->crtc_hsync_end = p->hsync_end;
	p->crtc_htotal = p->htotal;
	p->crtc_hskew = p->hskew;
	p->crtc_vdisplay = p->vdisplay;
	p->crtc_vsync_start = p->vsync_start;
	p->crtc_vsync_end = p->vsync_end;
	p->crtc_vtotal = p->vtotal;

	if (p->flags & DRM_MODE_FLAG_INTERLACE) {
		if (adjust_flags & CRTC_INTERLACE_HALVE_V) {
			p->crtc_vdisplay /= 2;
			p->crtc_vsync_start /= 2;
			p->crtc_vsync_end /= 2;
			p->crtc_vtotal /= 2;
		}
	}

	if (!(adjust_flags & CRTC_NO_DBLSCAN)) {
		if (p->flags & DRM_MODE_FLAG_DBLSCAN) {
			p->crtc_vdisplay *= 2;
			p->crtc_vsync_start *= 2;
			p->crtc_vsync_end *= 2;
			p->crtc_vtotal *= 2;
		}
	}

	if (!(adjust_flags & CRTC_NO_VSCAN)) {
		if (p->vscan > 1) {
			p->crtc_vdisplay *= p->vscan;
			p->crtc_vsync_start *= p->vscan;
			p->crtc_vsync_end *= p->vscan;
			p->crtc_vtotal *= p->vscan;
		}
	}

	if (adjust_flags & CRTC_STEREO_DOUBLE) {
		unsigned int layout = p->flags & DRM_MODE_FLAG_3D_MASK;

		switch (layout) {
		case DRM_MODE_FLAG_3D_FRAME_PACKING:
			p->crtc_clock *= 2;
			p->crtc_vdisplay += p->crtc_vtotal;
			p->crtc_vsync_start += p->crtc_vtotal;
			p->crtc_vsync_end += p->crtc_vtotal;
			p->crtc_vtotal += p->crtc_vtotal;
			break;
		}
	}

	p->crtc_vblank_start = min(p->crtc_vsync_start, p->crtc_vdisplay);
	p->crtc_vblank_end = max(p->crtc_vsync_end, p->crtc_vtotal);
	p->crtc_hblank_start = min(p->crtc_hsync_start, p->crtc_hdisplay);
	p->crtc_hblank_end = max(p->crtc_hsync_end, p->crtc_htotal);
}

/**
 * drm_mode_is_420_only - if a given videomode can be only supported in YCBCR420
 * output format
 *
 * @connector: drm connector under action.
 * @mode: video mode to be tested.
 *
 * Returns:
 * true if the mode can be supported in YCBCR420 format
 * false if not.
 */
bool drm_mode_is_420_only(const struct drm_display_info *display,
			  const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);

	return test_bit(vic, display->hdmi.y420_vdb_modes);
}

/**
 * drm_mode_is_420_also - if a given videomode can be supported in YCBCR420
 * output format also (along with RGB/YCBCR444/422)
 *
 * @display: display under action.
 * @mode: video mode to be tested.
 *
 * Returns:
 * true if the mode can be support YCBCR420 format
 * false if not.
 */
bool drm_mode_is_420_also(const struct drm_display_info *display,
			  const struct drm_display_mode *mode)
{
	u8 vic = drm_match_cea_mode(mode);

	return test_bit(vic, display->hdmi.y420_cmdb_modes);
}

/**
 * drm_mode_is_420 - if a given videomode can be supported in YCBCR420
 * output format
 *
 * @display: display under action.
 * @mode: video mode to be tested.
 *
 * Returns:
 * true if the mode can be supported in YCBCR420 format
 * false if not.
 */
bool drm_mode_is_420(const struct drm_display_info *display,
		     const struct drm_display_mode *mode)
{
	return drm_mode_is_420_only(display, mode) ||
		drm_mode_is_420_also(display, mode);
}

static int display_get_timing(struct display_state *state)
{
	const struct rockchip_connector *conn = state->conn_state.connector;
	struct connector_state *conn_state = &state->conn_state;
	const struct rockchip_connector_funcs *conn_funcs = conn->funcs;
	struct drm_display_mode *mode = &conn_state->mode;
	const struct drm_display_mode *m;
	const void *blob = state->blob;
	int conn_node = conn_state->node;
	int panel;

	panel = get_panel_node(state, conn_node);
	if (panel < 0) {
		printf("failed to find panel node\n");
		return -ENODEV;
	}

	if (!display_get_timing_from_dts(panel, blob, mode)) {
		printf("Using display timing dts\n");
		goto done;
	}

	m = rockchip_get_display_mode_from_panel(state);
	if (m) {
		printf("Using display timing from compatible panel driver\n");
		memcpy(mode, m, sizeof(*m));
		goto done;
	}

	rockchip_panel_prepare(state);

	if (conn_funcs->get_edid && !conn_funcs->get_edid(state)) {
		int panel_bits_per_colourp;

		if (!edid_get_drm_mode((void *)&conn_state->edid,
				       sizeof(conn_state->edid), mode,
				       &panel_bits_per_colourp)) {
			printf("Using display timing from edid\n");
			edid_print_info((void *)&conn_state->edid);
			goto done;
		}
	}

	printf("failed to find display timing\n");
	return -ENODEV;
done:
	printf("Detailed mode clock %u kHz, flags[%x]\n"
	       "    H: %04d %04d %04d %04d\n"
	       "    V: %04d %04d %04d %04d\n"
	       "bus_format: %x\n",
	       mode->clock, mode->flags,
	       mode->hdisplay, mode->hsync_start,
	       mode->hsync_end, mode->htotal,
	       mode->vdisplay, mode->vsync_start,
	       mode->vsync_end, mode->vtotal,
	       conn_state->bus_format);

	return 0;
}

static int display_init(struct display_state *state)
{
	const struct rockchip_connector *conn = state->conn_state.connector;
	const struct rockchip_connector_funcs *conn_funcs = conn->funcs;
	struct rockchip_crtc *crtc = state->crtc_state.crtc;
	const struct rockchip_crtc_funcs *crtc_funcs = crtc->funcs;
	const struct connector_state *conn_state = &state->conn_state;
	struct drm_display_mode *mode = &conn_state->mode;
	int ret = 0;

	if (state->is_init)
		return 0;

	if (!conn_funcs || !crtc_funcs) {
		printf("failed to find connector or crtc functions\n");
		return -ENXIO;
	}

	if (conn_funcs->init) {
		ret = conn_funcs->init(state);
		if (ret)
			goto deinit_panel;
	}
	/*
	 * support hotplug, but not connect;
	 */

#ifdef CONFIG_ROCKCHIP_DRM_TVE
	if (crtc->hdmi_hpd && conn_state->type == DRM_MODE_CONNECTOR_TV) {
		printf("hdmi plugin ,skip tve\n");
		goto deinit;
	}
#elif defined(CONFIG_ROCKCHIP_DRM_RK1000)
	if (crtc->hdmi_hpd && conn_state->type == DRM_MODE_CONNECTOR_LVDS) {
		printf("hdmi plugin ,skip tve\n");
		goto deinit;
	}
#endif
	if (conn_funcs->detect) {
		ret = conn_funcs->detect(state);

#if defined(CONFIG_ROCKCHIP_DRM_TVE) || defined(CONFIG_ROCKCHIP_DRM_RK1000)
		if (conn_state->type == DRM_MODE_CONNECTOR_HDMIA)
			crtc->hdmi_hpd = ret;
#endif
		if (!ret)
			goto deinit;
	}

	if (conn_funcs->get_timing) {
		ret = conn_funcs->get_timing(state);
		if (ret)
			goto deinit;
	} else {
		ret = display_get_timing(state);
		if (ret)
			goto deinit;
	}
	drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V);

	if (crtc_funcs->init) {
		ret = crtc_funcs->init(state);
		if (ret)
			goto deinit;
	}

	state->is_init = 1;

	return 0;

deinit:
	if (conn_funcs->deinit)
		conn_funcs->deinit(state);
deinit_panel:
	rockchip_panel_deinit(state);
	return ret;
}

static int display_set_plane(struct display_state *state)
{
	const struct rockchip_crtc *crtc = state->crtc_state.crtc;
	const struct rockchip_crtc_funcs *crtc_funcs = crtc->funcs;
	int ret;

	if (!state->is_init)
		return -EINVAL;

	if (crtc_funcs->set_plane) {
		ret = crtc_funcs->set_plane(state);
		if (ret)
			return ret;
	}

	return 0;
}

static int display_enable(struct display_state *state)
{
	const struct rockchip_connector *conn = state->conn_state.connector;
	const struct rockchip_crtc *crtc = state->crtc_state.crtc;
	const struct rockchip_connector_funcs *conn_funcs = conn->funcs;
	const struct rockchip_crtc_funcs *crtc_funcs = crtc->funcs;
	int ret = 0;

	display_init(state);

	if (!state->is_init)
		return -EINVAL;

	if (state->is_enable)
		return 0;

	if (crtc_funcs->prepare) {
		ret = crtc_funcs->prepare(state);
		if (ret)
			return ret;
	}

	if (conn_funcs->prepare) {
		ret = conn_funcs->prepare(state);
		if (ret)
			goto unprepare_crtc;
	}

	rockchip_panel_prepare(state);

	if (crtc_funcs->enable) {
		ret = crtc_funcs->enable(state);
		if (ret)
			goto unprepare_conn;
	}

	if (conn_funcs->enable) {
		ret = conn_funcs->enable(state);
		if (ret)
			goto disable_crtc;
	}

	rockchip_panel_enable(state);

	state->is_enable = true;

	return 0;
unprepare_crtc:
	if (crtc_funcs->unprepare)
		crtc_funcs->unprepare(state);
unprepare_conn:
	if (conn_funcs->unprepare)
		conn_funcs->unprepare(state);
disable_crtc:
	if (crtc_funcs->disable)
		crtc_funcs->disable(state);
	return ret;
}

static int display_disable(struct display_state *state)
{
	const struct rockchip_connector *conn = state->conn_state.connector;
	const struct rockchip_crtc *crtc = state->crtc_state.crtc;
	const struct rockchip_connector_funcs *conn_funcs = conn->funcs;
	const struct rockchip_crtc_funcs *crtc_funcs = crtc->funcs;

	if (!state->is_init)
		return 0;

	if (!state->is_enable)
		return 0;

	rockchip_panel_disable(state);

	if (crtc_funcs->disable)
		crtc_funcs->disable(state);

	if (conn_funcs->disable)
		conn_funcs->disable(state);

	rockchip_panel_unprepare(state);

	if (conn_funcs->unprepare)
		conn_funcs->unprepare(state);

	state->is_enable = 0;
	state->is_init = 0;

	return 0;
}

static int display_logo(struct display_state *state)
{
	struct crtc_state *crtc_state = &state->crtc_state;
	struct connector_state *conn_state = &state->conn_state;
	struct logo_info *logo = &state->logo;
	int hdisplay, vdisplay;
	unsigned long fb_start;
	unsigned long fb_size;
	int ret;

	display_init(state);
	if (!state->is_init)
		return -ENODEV;

	switch (logo->bpp) {
	case 16:
		crtc_state->format = ROCKCHIP_FMT_RGB565;
		break;
	case 24:
		crtc_state->format = ROCKCHIP_FMT_RGB888;
		break;
	case 32:
		crtc_state->format = ROCKCHIP_FMT_ARGB8888;
		break;
	default:
		printf("can't support bmp bits[%d]\n", logo->bpp);
		return -EINVAL;
	}
	crtc_state->rb_swap = logo->bpp != 32;
	hdisplay = conn_state->mode.hdisplay;
	vdisplay = conn_state->mode.vdisplay;
	crtc_state->src_w = logo->width;
	crtc_state->src_h = logo->height;
	crtc_state->src_x = 0;
	crtc_state->src_y = 0;
	crtc_state->ymirror = logo->ymirror;

	crtc_state->dma_addr = (u32)(unsigned long)(logo->mem + logo->offset);
	crtc_state->xvir = ALIGN(crtc_state->src_w * logo->bpp, 32) >> 5;

	if (logo->mode == ROCKCHIP_DISPLAY_FULLSCREEN) {
		crtc_state->crtc_x = 0;
		crtc_state->crtc_y = 0;
		crtc_state->crtc_w = hdisplay;
		crtc_state->crtc_h = vdisplay;
	} else {
		if (crtc_state->src_w >= hdisplay) {
			crtc_state->crtc_x = 0;
			crtc_state->crtc_w = hdisplay;
		} else {
			crtc_state->crtc_x = (hdisplay - crtc_state->src_w) / 2;
			crtc_state->crtc_w = crtc_state->src_w;
		}

		if (crtc_state->src_h >= vdisplay) {
			crtc_state->crtc_y = 0;
			crtc_state->crtc_h = vdisplay;
		} else {
			crtc_state->crtc_y = (vdisplay - crtc_state->src_h) / 2;
			crtc_state->crtc_h = crtc_state->src_h;
		}
	}

	ret = rockchip_calc_fb_size(crtc_state->src_w, crtc_state->src_h,
				    logo->bpp, &fb_size);
	if (ret)
		return ret;

	fb_start = (unsigned long)crtc_state->dma_addr;
	flush_dcache_range(fb_start,
			   ALIGN(fb_start + fb_size, ARCH_DMA_MINALIGN));

	ret = display_set_plane(state);
	if (ret)
		return ret;

	return display_enable(state);
}

static int get_crtc_id(const void *blob, int connect)
{
	int phandle, remote;
	int val;

	phandle = fdt_getprop_u32_default_node(blob, connect, 0,
					       "remote-endpoint", -1);
	if (phandle < 0)
		goto err;
	remote = fdt_node_offset_by_phandle(blob, phandle);

	val = fdtdec_get_int(blob, remote, "reg", -1);
	if (val < 0)
		goto err;

	return val;
err:
	printf("Can't get crtc id, default set to id = 0\n");
	return 0;
}

static int find_crtc_node(const void *blob, int node)
{
	int nodedepth = fdt_node_depth(blob, node);

	if (nodedepth < 2)
		return -EINVAL;

	return fdt_supernode_atdepth_offset(blob, node,
					    nodedepth - 2, NULL);
}

static int find_connector_node(const void *blob, int node)
{
	int phandle, remote;
	int nodedepth;

	phandle = fdt_getprop_u32_default_node(blob, node, 0,
					       "remote-endpoint", -1);
	remote = fdt_node_offset_by_phandle(blob, phandle);
	nodedepth = fdt_node_depth(blob, remote);

	return fdt_supernode_atdepth_offset(blob, remote,
					    nodedepth - 3, NULL);
}

struct rockchip_logo_cache *find_or_alloc_logo_cache(const char *bmp, u32 rotate)
{
	struct rockchip_logo_cache *tmp, *logo_cache = NULL;

	list_for_each_entry(tmp, &logo_cache_list, head) {
		if (!strcmp(tmp->name, bmp) && tmp->logo.rotate == rotate) {
			logo_cache = tmp;
			break;
		}
	}

	if (!logo_cache) {
		logo_cache = malloc(sizeof(*logo_cache));
		if (!logo_cache) {
			printf("failed to alloc memory for logo cache\n");
			return NULL;
		}
		memset(logo_cache, 0, sizeof(*logo_cache));
		logo_cache->name = strdup(bmp);
		if (!logo_cache->name) {
			free(logo_cache);
			return NULL;
		}
		INIT_LIST_HEAD(&logo_cache->head);
		list_add_tail(&logo_cache->head, &logo_cache_list);
	}

	return logo_cache;
}

static int load_bmp_logo(struct logo_info *logo, const char *bmp_name)
{
	struct rockchip_logo_cache *logo_cache;
	struct bmp_header *header;
	void *dst = NULL, *pdst;
	unsigned long size;
	unsigned long dst_size;
	u32 data_offset;
	int ret;

	if (!logo || !bmp_name)
		return -EINVAL;
	logo_cache = find_or_alloc_logo_cache(bmp_name, logo->rotate);
	if (!logo_cache)
		return -ENOMEM;

	if (logo_cache->logo.mem) {
		memcpy(logo, &logo_cache->logo, sizeof(*logo));
		logo->mode = logo_cache->logo.mode;
		return 0;
	}

	header = get_bmp_header(bmp_name);
	if (!header)
		return -EINVAL;

	logo->bpp = get_unaligned_le16(&header->bit_count);
	logo->width = get_unaligned_le32(&header->width);
	logo->height = get_unaligned_le32(&header->height);
	size = get_unaligned_le32(&header->file_size);
	data_offset = get_unaligned_le32(&header->data_offset);
	if (!size || size < sizeof(*header) || !logo->width || !logo->height)
		return -EINVAL;
	if (logo->width > INT_MAX || logo->height > INT_MAX)
		return -EOVERFLOW;
	if (data_offset >= size)
		return -EINVAL;
	if (!can_direct_logo(logo->bpp)) {
		if (size > CONFIG_RK_BOOT_BUFFER_SIZE) {
			printf("failed to use boot buf as temp bmp buffer\n");
			return -ENOMEM;
		}
		pdst = (void *)gd->arch.rk_boot_buf_addr;

	} else {
		pdst = get_display_buffer(size);
		dst = pdst;
	}

	if (load_bmp_content(bmp_name, pdst, size)) {
		printf("failed to load bmp %s\n", bmp_name);
		return -EINVAL;
	}

	if (!can_direct_logo(logo->bpp)) {
		/*
		 * TODO: force use 16bpp if bpp less than 16;
		 */
		logo->bpp = (logo->bpp <= 16) ? 16 : logo->bpp;
		ret = rockchip_calc_fb_size(logo->width, logo->height, logo->bpp,
					    &dst_size);
		if (ret)
			return ret;

		dst = get_display_buffer(dst_size);
		if (!dst)
			return -ENOMEM;
		if (bmpdecoder(pdst, dst, logo->bpp)) {
			printf("failed to decode bmp %s\n", bmp_name);
			return -EINVAL;
		}
		logo->offset = 0;
		logo->ymirror = 0;
	} else {
		logo->offset = data_offset;
		logo->ymirror = 1;
	}

	logo->mem = dst;
	rockchip_recolor_logo(logo);
	rockchip_prepare_logo(logo);

	memcpy(&logo_cache->logo, logo, sizeof(*logo));

	return 0;
}

void rockchip_show_bmp(const char *bmp)
{
	struct display_state *s;

	if (!bmp) {
		list_for_each_entry(s, &rockchip_display_list, head)
			display_disable(s);
		return;
	}

	list_for_each_entry(s, &rockchip_display_list, head) {
		s->logo.mode = s->charge_logo_mode;
		s->logo.rotate = s->rotate;
		if (load_bmp_logo(&s->logo, bmp))
			continue;
		display_logo(s);
	}
}

#ifndef CONFIG_LCD_CONSOLE_DISABLE
/* Persistent framebuffer for post-splash updates (e.g. fastboot screen) */
static void *g_splash_fb;
static void *g_splash_scanout_fb;
static int g_splash_fb_w;
static int g_splash_fb_h;
static int g_splash_scanout_w;
static int g_splash_scanout_h;
static u32 g_splash_rotate;
static struct display_state *g_splash_state;

static void rockchip_flush_logo_buffer(void *fb, int w, int h, int bpp)
{
	unsigned long start = (unsigned long)fb;
	unsigned long size;

	if (!fb || w <= 0 || h <= 0 || !bpp)
		return;

	size = (unsigned long)logo_stride_bytes(w, bpp) * h;
	flush_dcache_range(start, ALIGN(start + size, ARCH_DMA_MINALIGN));
}

static void rockchip_refresh_splash(struct display_state *s)
{
	struct logo_info logical_logo;

	if (!g_splash_fb || g_splash_fb_w <= 0 || g_splash_fb_h <= 0)
		return;

	if (g_splash_rotate == 90 || g_splash_rotate == 270) {
		memset(&logical_logo, 0, sizeof(logical_logo));
		logical_logo.mem = g_splash_fb;
		logical_logo.width = g_splash_fb_w;
		logical_logo.height = g_splash_fb_h;
		logical_logo.bpp = 32;
		logical_logo.rotate = g_splash_rotate;
		s->logo.mem = g_splash_scanout_fb;
		rockchip_rotate_logo(&logical_logo, &s->logo, g_splash_rotate);
		if (!s->logo.mem)
			return;
		rockchip_flush_logo_buffer(g_splash_scanout_fb,
					   g_splash_scanout_w,
					   g_splash_scanout_h, 32);
	} else {
		rockchip_flush_logo_buffer(g_splash_fb, g_splash_fb_w,
					   g_splash_fb_h, 32);
	}
}

static void rockchip_draw_text_line(void *fb, int fb_w, int fb_h,
				    int x, int y, const char *text, u32 color)
{
	u32 *pixels = (u32 *)fb;
	int row, col;

	while (*text && x < fb_w) {
		unsigned char c = (unsigned char)*text++;
		const unsigned char *glyph =
			video_fontdata + c * VIDEO_FONT_HEIGHT;

		for (row = 0; row < VIDEO_FONT_HEIGHT && (y + row) < fb_h;
		     row++) {
			unsigned char bits = glyph[row];

			for (col = 0; col < VIDEO_FONT_WIDTH &&
			     (x + col) < fb_w; col++) {
				if (bits & (0x80 >> col))
					pixels[(y + row) * fb_w + (x + col)] =
						color;
			}
		}
		x += VIDEO_FONT_WIDTH;
	}
}

static int rockchip_prepare_splash_fb(struct display_state *s, bool display_now)
{
	struct connector_state *conn_state = &s->conn_state;
	int w, h;
	unsigned long size;
	unsigned long scanout_size;
	void *fb;

	display_init(s);
	if (!s->is_init) {
		printf("rockchip_prepare_splash_fb: display_init failed\n");
		return -ENODEV;
	}

	w = conn_state->mode.hdisplay;
	h = conn_state->mode.vdisplay;
	if (s->rotate == 90 || s->rotate == 270) {
		int tmp = w;
		w = h;
		h = tmp;
	}
	if (w <= 0 || h <= 0)
		return -EINVAL;

	if (rockchip_calc_fb_size(w, h, 32, &size))
		return -EOVERFLOW;
	fb = get_display_buffer(size);
	if (!fb) {
		printf("rockchip_prepare_splash_fb: get_display_buffer(%lu) failed\n",
		       size);
		return -ENOMEM;
	}
	rockchip_fill_argb8888(fb, w * h, 0xFF000000); /* opaque black background */

	s->logo.mode = ROCKCHIP_DISPLAY_FULLSCREEN;
	s->logo.bpp = 32;
	s->logo.width = w;
	s->logo.height = h;
	s->logo.mem = (char *)(unsigned long)fb;
	s->logo.offset = 0;
	s->logo.ymirror = 0;
	s->logo.rotate = s->rotate;
	g_splash_rotate = s->rotate;
	if (s->rotate == 90 || s->rotate == 270) {
		if (rockchip_calc_fb_size(conn_state->mode.hdisplay,
					  conn_state->mode.vdisplay, 32,
					  &scanout_size))
			return -EOVERFLOW;
		g_splash_scanout_fb = get_display_buffer(scanout_size);
		if (!g_splash_scanout_fb) {
			printf("rockchip_prepare_splash_fb: get_display_buffer(scanout) failed\n");
			return -ENOMEM;
		}
		g_splash_scanout_w = conn_state->mode.hdisplay;
		g_splash_scanout_h = conn_state->mode.vdisplay;
	} else {
		g_splash_scanout_fb = fb;
		g_splash_scanout_w = w;
		g_splash_scanout_h = h;
	}

	/* Store for later updates (fastboot screen) */
	g_splash_fb = fb;
	g_splash_fb_w = w;
	g_splash_fb_h = h;
	g_splash_state = s;

	if (display_now) {
		rockchip_refresh_splash(s);
		display_logo(s);
	}

	return 0;
}

static void rockchip_show_splash(struct display_state *s)
{
	int cx, cy;

	if (rockchip_prepare_splash_fb(s, false))
		return;

	/* "LHC Rocks" centered */
	cx = (g_splash_fb_w - (int)strlen("LHC Rocks") * VIDEO_FONT_WIDTH) / 2;
	cy = g_splash_fb_h / 2 - VIDEO_FONT_HEIGHT;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	rockchip_draw_text_line(g_splash_fb, g_splash_fb_w, g_splash_fb_h,
				cx, cy, "LHC Rocks", 0xFFFFFFFF);

	/* U-Boot version below */
	cx = (g_splash_fb_w - (int)strlen(U_BOOT_VERSION) * VIDEO_FONT_WIDTH) / 2;
	cy += VIDEO_FONT_HEIGHT * 2;
	if (cx < 0) cx = 0;
	rockchip_draw_text_line(g_splash_fb, g_splash_fb_w, g_splash_fb_h,
				cx, cy, U_BOOT_VERSION, 0xFF808080);

	rockchip_refresh_splash(s);
	display_logo(s);
}

#ifndef CONFIG_LCD_CONSOLE_DISABLE
static const char *rockchip_fastboot_unlocked_state(void)
{
	char *env = getenv(FASTBOOT_UNLOCKED_ENV_NAME);

	return (env && !strcmp(env, "1")) ? "unlocked" : "locked";
}

static const char *rockchip_boot_media_name(uint16 media)
{
	switch (media) {
	case BOOT_FROM_FLASH:
		return "nand";
	case BOOT_FROM_EMMC:
		return "emmc";
	case BOOT_FROM_SD0:
		return "sdcard";
	case BOOT_FROM_SD1:
		return "sdcard1";
	case BOOT_FROM_SPI:
		return "spi";
	case BOOT_FROM_UMS:
		return "usb-ums";
	case BOOT_FROM_NVME:
		return "nvme";
	default:
		return "unknown";
	}
}

static const char *rockchip_reboot_reason_name(enum fbt_reboot_type reboot_type)
{
	switch (reboot_type) {
	case FASTBOOT_REBOOT_NORMAL:
		return "normal";
	case FASTBOOT_REBOOT_BOOTLOADER:
		return "bootloader";
	case FASTBOOT_REBOOT_RECOVERY:
		return "recovery";
	case FASTBOOT_REBOOT_RECOVERY_WIPE_DATA:
		return "recovery-wipe";
	case FASTBOOT_REBOOT_NORECOVER:
		return "no-recover";
	case FASTBOOT_REBOOT_FASTBOOT:
		return "fastboot";
	case FASTBOOT_REBOOT_CHARGE:
		return "charge";
	case FASTBOOT_REBOOT_UNKNOWN:
	default:
		return "cold-boot";
	}
}

static const char *rockchip_soc_name(void)
{
#ifdef CONFIG_RKCHIP_RK3288
	return "RK3288";
#else
	return "Rockchip";
#endif
}
#endif

void rockchip_show_fastboot_screen(const char *serial, const char *product,
				   const char *bootloader,
				   enum fbt_reboot_type reboot_type)
{
	char line[96];
	int y, x, lw;
	const int SCALE = 2; /* font scale factor (pixels per font pixel) */
	void *fb;
	int w, h;
	const char *unlocked;
	const char *secure_state;
	const char *media_name;
	const char *reason_name;
	const char *header = "FASTBOOT MODE";

	if ((!g_splash_fb || g_splash_fb_w <= 0 || g_splash_fb_h <= 0) &&
	    !list_empty(&rockchip_display_list) &&
	    rockchip_prepare_splash_fb(list_first_entry(&rockchip_display_list,
							struct display_state,
							head), false))
		return;

	fb = g_splash_fb;
	w = g_splash_fb_w;
	h = g_splash_fb_h;

	if (!fb || w <= 0 || h <= 0)
		return;

	rockchip_fill_argb8888(fb, w * h, 0xFF000000); /* opaque black background */
	unlocked = rockchip_fastboot_unlocked_state();
	secure_state = SecureBootEn ? (SecureBootLock ? "enabled, locked" :
					"enabled, unlocked") : "disabled";
	media_name = rockchip_boot_media_name(StorageGetBootMedia());
	reason_name = rockchip_reboot_reason_name(reboot_type);

	/* Header */
	y = h / 8;
	lw = strlen(header) * VIDEO_FONT_WIDTH;
	x = (w - lw * SCALE) / 2;
	if (x < 0) x = 4;
	/* Draw at 2x scale by writing each glyph pixel as 2x2 block */
	{
		u32 *pixels = (u32 *)fb;
		const char *hdr = header;
		int row, col, px, py;
		int sx = x;

		while (*hdr) {
			unsigned char c = (unsigned char)*hdr++;
			const unsigned char *glyph =
				video_fontdata + c * VIDEO_FONT_HEIGHT;

			for (row = 0; row < VIDEO_FONT_HEIGHT; row++) {
				unsigned char bits = glyph[row];

				for (col = 0; col < VIDEO_FONT_WIDTH; col++) {
					if (bits & (0x80 >> col)) {
						for (py = 0; py < SCALE; py++)
						for (px = 0; px < SCALE; px++) {
							int fy = y + row * SCALE + py;
							int fx = sx + col * SCALE + px;
							if (fy < h && fx < w)
								pixels[fy * w + fx] = 0xFF00AAFF;
						}
					}
				}
			}
			sx += VIDEO_FONT_WIDTH * SCALE;
		}
	}

	y = h / 8 + VIDEO_FONT_HEIGHT * SCALE + 16;
	x = 24;

	rockchip_draw_text_line(fb, w, h, x, y, "Android Fastboot", 0xFF00AAFF);
	y += VIDEO_FONT_HEIGHT + 6;

	snprintf(line, sizeof(line), "Product:    %s", product ? product : "unknown");
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Bootloader: %s", bootloader ? bootloader : "unknown");
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Serial:     %s", serial ? serial : "unknown");
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Device:     %s", unlocked);
	rockchip_draw_text_line(fb, w, h, x, y, line,
				!strcmp(unlocked, "unlocked") ? 0xFF00FF00 : 0xFFFFC857);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Secure:     %s", secure_state);
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 10;

	rockchip_draw_text_line(fb, w, h, x, y, "Rockchip Details", 0xFF7FB3FF);
	y += VIDEO_FONT_HEIGHT + 6;

	snprintf(line, sizeof(line), "SoC:        %s", rockchip_soc_name());
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Boot media: %s", media_name);
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	snprintf(line, sizeof(line), "Reason:     %s", reason_name);
	rockchip_draw_text_line(fb, w, h, x, y, line, 0xFFFFFFFF);
	y += VIDEO_FONT_HEIGHT + 4;

	y += VIDEO_FONT_HEIGHT + 8;

	rockchip_draw_text_line(fb, w, h, x, y,
				"Waiting for fastboot commands...", 0xFFAAAAAA);
	if (g_splash_state)
		rockchip_refresh_splash(g_splash_state);
	if (g_splash_state)
		display_logo(g_splash_state);
}
#endif /* CONFIG_LCD_CONSOLE_DISABLE */

void rockchip_show_logo(void)
{
	struct display_state *s;

	list_for_each_entry(s, &rockchip_display_list, head) {
		s->logo.mode = s->logo_mode;
		s->logo.rotate = s->rotate;
		if (load_bmp_logo(&s->logo, s->ulogo_name)) {
#ifndef CONFIG_LCD_CONSOLE_DISABLE
			rockchip_show_splash(s);
#endif
		} else {
			display_logo(s);
		}
		if (load_bmp_logo(&s->logo, s->klogo_name))
			printf("failed to display kernel logo\n");
	}
}

int rockchip_display_init(void)
{
	const void *blob = gd->fdt_blob;
	int route, child, phandle, connect, crtc_node, conn_node;
	const struct rockchip_connector *conn;
	const struct rockchip_crtc *crtc;
	struct display_state *s;
	struct public_phy_data *data;
	const char *name;

	printf("Rockchip UBOOT DRM driver version: %s\n", DRIVER_VERSION);

	route = fdt_path_offset(blob, "/display-subsystem/route");
	if (route < 0) {
		printf("Can't find display display route node\n");
		return -ENODEV;
	}

	if (!fdt_device_is_available(blob, route))
		return -ENODEV;

	data = malloc(sizeof(struct public_phy_data));
	if (!data) {
		printf("failed to alloc phy data\n");
		return -ENOMEM;
	}
	data->phy_init = false;
	init_display_buffer();

	fdt_for_each_subnode(blob, child, route) {
		if (!fdt_device_is_available(blob, child))
			continue;

		phandle = fdt_getprop_u32_default_node(blob, child, 0,
						       "connect", -1);
		if (phandle < 0) {
			printf("Warn: %s: can't find connect node's handle\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}

		connect = fdt_node_offset_by_phandle(blob, phandle);
		if (connect < 0) {
			printf("Warn: %s: can't find connect node\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}

		crtc_node = find_crtc_node(blob, connect);
		if (!fdt_device_is_available(blob, crtc_node)) {
			printf("Warn: %s: crtc node is not available\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}
		crtc = rockchip_get_crtc(blob, crtc_node);
		if (!crtc) {
			printf("Warn: %s: can't find crtc driver\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}

		conn_node = find_connector_node(blob, connect);
		if (!fdt_device_is_available(blob, conn_node)) {
			printf("Warn: %s: connector node is not available\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}

		conn = rockchip_get_connector(blob, conn_node);
		if (!conn) {
			printf("Warn: %s: can't find connector driver\n",
			       fdt_get_name(blob, child, NULL));
			continue;
		}

		s = malloc(sizeof(*s));
		if (!s)
			goto err_free;

		memset(s, 0, sizeof(*s));

		INIT_LIST_HEAD(&s->head);
		fdt_get_string(blob, child, "logo,uboot", &s->ulogo_name);
		fdt_get_string(blob, child, "logo,kernel", &s->klogo_name);
		fdt_get_string(blob, child, "logo,mode", &name);
		if (!strcmp(name, "fullscreen"))
			s->logo_mode = ROCKCHIP_DISPLAY_FULLSCREEN;
		else
			s->logo_mode = ROCKCHIP_DISPLAY_CENTER;
		fdt_get_string(blob, child, "charge_logo,mode", &name);
		if (!strcmp(name, "fullscreen"))
			s->charge_logo_mode = ROCKCHIP_DISPLAY_FULLSCREEN;
		else
			s->charge_logo_mode = ROCKCHIP_DISPLAY_CENTER;
		s->rotate = fdtdec_get_int(blob, child, "logo,rotate", 0);

		s->blob = blob;
		s->conn_state.node = conn_node;
		s->conn_state.connector = conn;
		s->conn_state.overscan.left_margin = 100;
		s->conn_state.overscan.right_margin = 100;
		s->conn_state.overscan.top_margin = 100;
		s->conn_state.overscan.bottom_margin = 100;
		s->crtc_state.node = crtc_node;
		s->crtc_state.crtc = crtc;
		s->crtc_state.crtc_id = get_crtc_id(blob, connect);
		s->node = child;

		connector_phy_init(s, data);
		connector_panel_init(s);
		list_add_tail(&s->head, &rockchip_display_list);
	}

	return 0;

err_free:
	list_for_each_entry(s, &rockchip_display_list, head) {
		list_del(&s->head);
		free(s);
	}
	free(data);
	return -ENODEV;
}

void rockchip_display_fixup(void *blob)
{
	const struct rockchip_connector_funcs *conn_funcs;
	const struct rockchip_crtc_funcs *crtc_funcs;
	const struct rockchip_connector *conn;
	const struct rockchip_crtc *crtc;
	struct display_state *s;
	u32 offset;
	int node;
	char path[100];
	int ret;

	if (!get_display_size())
		return;

	node = fdt_update_reserved_memory(blob, "rockchip,drm-logo",
					       (u64)memory_start,
					       (u64)get_display_size());
	if (node < 0) {
		printf("failed to add drm-loader-logo memory\n");
		return;
	}

	list_for_each_entry(s, &rockchip_display_list, head) {
		conn = s->conn_state.connector;
		if (!conn)
			continue;
		conn_funcs = conn->funcs;
		if (!conn_funcs) {
			printf("failed to get exist connector\n");
			continue;
		}

		crtc = s->crtc_state.crtc;
		if (!crtc)
			continue;

		crtc_funcs = crtc->funcs;
		if (!crtc_funcs) {
			printf("failed to get exist crtc\n");
			continue;
		}

		if (crtc_funcs->fixup_dts)
			crtc_funcs->fixup_dts(s, blob);

		if (conn_funcs->fixup_dts)
			conn_funcs->fixup_dts(s, blob);

		ret = fdt_get_path(s->blob, s->node, path, sizeof(path));
		if (ret < 0) {
			printf("failed to get route path[%s], ret=%d\n",
			       path, ret);
			continue;
		}

#define FDT_SET_U32(name, val) \
		do_fixup_by_path_u32(blob, path, name, val, 1);

		offset = s->logo.offset + (unsigned long)s->logo.mem - memory_start;
		FDT_SET_U32("logo,offset", offset);
		FDT_SET_U32("logo,width", s->logo.width);
		FDT_SET_U32("logo,height", s->logo.height);
		FDT_SET_U32("logo,bpp", s->logo.bpp);
		FDT_SET_U32("logo,ymirror", s->logo.ymirror);
		FDT_SET_U32("logo,rotate", s->rotate);
		FDT_SET_U32("video,hdisplay", s->conn_state.mode.hdisplay);
		FDT_SET_U32("video,vdisplay", s->conn_state.mode.vdisplay);
		FDT_SET_U32("video,crtc_hsync_end", s->conn_state.mode.crtc_hsync_end);
		FDT_SET_U32("video,crtc_vsync_end", s->conn_state.mode.crtc_vsync_end);
		FDT_SET_U32("video,vrefresh",
			    drm_mode_vrefresh(&s->conn_state.mode));
		FDT_SET_U32("video,flags", s->conn_state.mode.flags);
		FDT_SET_U32("overscan,left_margin", s->conn_state.overscan.left_margin);
		FDT_SET_U32("overscan,right_margin", s->conn_state.overscan.right_margin);
		FDT_SET_U32("overscan,top_margin", s->conn_state.overscan.top_margin);
		FDT_SET_U32("overscan,bottom_margin", s->conn_state.overscan.bottom_margin);
#undef FDT_SET_U32
	}
}
