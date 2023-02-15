// SPDX-License-Identifier: GPL-2.0
/*
 * DRM Video Endpoint driver.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drmP.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/phy/phy.h>
#include <video/videomode.h>
#include "xlnx_sdi_modes.h"
#include "xlnx_sdi_timing.h"

#include "xlnx_bridge.h"

/* DRM Video Endpoint register offsets */
#define RREG_VERSION                   0x00
#define RREG_IRQ_STATUS                0x04
#define RREG_AXI_S_VID_LOCKED          0x08
#define RREG_AXI_S_VID_OVERFLOW        0x0C
#define RREG_AXI_S_VID_UNDERFLOW       0x10
#define RREG_AXI_S_VID_STATUS          0x14
#define RREG_AXI_S_VID_FIFO_READ_LEVEL 0x18
#define RREG_SDI_BRIDGE_STATUS         0x1c

#define WREG_CORE_ENABLE               0x00
#define WREG_AXI_S_VID_ENABLE          0x04
#define WREG_IRQ_ENABLE                0x08
#define WREG_IRQ_CLEAR                 0x0C
#define WREG_IRQ_MASK                  0x10
#define WREG_SDI_BRIDGE_ENABLE         0x14
#define WREG_SDI_MODE                  0x18
#define WREG_SDI_IS_FRAC               0x1C
#define WREG_SDI_FORMAT                0x20

/* Interrupt enable register masks */
#define AXI_S_VID_LOCKED_INTR     BIT(0)
#define AXI_S_VID_OVERFLOW_INTR   BIT(1)
#define AXI_S_VID_UNDERFLOW_INTR	BIT(2)
#define IRQ_EN_MASK		(AXI_S_VID_OVERFLOW_INTR | \
                       AXI_S_VID_UNDERFLOW_INTR)

#define PIXELS_PER_CLK			2

/* SDI modes */
#define SDI_MODE_HD			0
#define	SDI_MODE_SD			1
#define	SDI_MODE_3GA			2
#define	SDI_MODE_3GB			3
#define	SDI_MODE_6G			4
#define	SDI_MODE_12G			5

#define SDI_TIMING_PARAMS_SIZE		48

/**
 * struct drm_vid_ep - Core configuration DRM Video Endpoint device structure
 * @encoder: DRM encoder structure
 * @connector: DRM connector structure
 * @dev: device structure
 * @base: Base address of SDI subsystem
 * @mode_flags: SDI operation mode related flags
 * @sdi_mode: configurable SDI mode parameter, supported values are:
 *		0 - HD
 *		1 - SD
 *		2 - 3GA
 *		3 - 3GB
 *		4 - 6G
 *		5 - 12G
 * @sdi_mod_prop_val: configurable SDI mode parameter value
 * @sdi_data_strm: configurable SDI data stream parameter
 * @sdi_data_strm_prop_val: configurable number of SDI data streams
 *			    value currently supported are 2, 4 and 8
 * @sdi_420_in: Specifying input bus color format parameter to SDI
 * @sdi_420_in_val: 1 for yuv420 and 0 for yuv422
 * @sdi_420_out: configurable SDI out color format parameter
 * @sdi_420_out_val: 1 for yuv420 and 0 for yuv422
 * @is_frac_prop: configurable SDI fractional fps parameter
 * @is_frac_prop_val: configurable SDI fractional fps parameter value
 * @bridge: bridge structure
 * @height_out: configurable bridge output height parameter
 * @height_out_prop_val: configurable bridge output height parameter value
 * @width_out: configurable bridge output width parameter
 * @width_out_prop_val: configurable bridge output width parameter value
 * @in_fmt: configurable bridge input media format
 * @in_fmt_prop_val: configurable media bus format value
 * @out_fmt: configurable bridge output media format
 * @out_fmt_prop_val: configurable media bus format value
 * @video_mode: current display mode
 * @axi_clk: AXI Lite interface clock
 * @vidin_clk: Video Clock
 */
struct drm_vid_ep {
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct device *dev;
	void __iomem *base;
	u32 mode_flags;
	struct drm_property *sdi_mode;
	u32 sdi_mod_prop_val;
	struct drm_property *sdi_data_strm;
	u32 sdi_data_strm_prop_val;
	struct drm_property *sdi_420_in;
	bool sdi_420_in_val;
	struct drm_property *sdi_420_out;
	bool sdi_420_out_val;
	struct drm_property *is_frac_prop;
	bool is_frac_prop_val;
	struct xlnx_bridge *bridge;
	struct drm_property *height_out;
	u32 height_out_prop_val;
	struct drm_property *width_out;
	u32 width_out_prop_val;
	struct drm_property *in_fmt;
	u32 in_fmt_prop_val;
	struct drm_property *out_fmt;
	u32 out_fmt_prop_val;
	struct drm_display_mode video_mode;
	struct clk *axi_clk;
	struct clk *vidin_clk;
};

#define connector_to_drm_vid_ep(c) container_of(c, struct drm_vid_ep, connector)
#define encoder_to_drm_vid_ep(e) container_of(e, struct drm_vid_ep, encoder)

static inline void drm_vid_ep_writel(void __iomem *base, int offset, u32 val)
{
	writel(val, base + offset);
}

static inline u32 drm_vid_ep_readl(void __iomem *base, int offset)
{
	return readl(base + offset);
}

/**
 * drm_vid_ep_enable_sdi_bridge - Enable Video to SDI bridge
 * @drm_ep:	Pointer to DRM Vid endpoint structure
 *
 * This function enables the Video to SDI bridge.
 */
static void drm_vid_ep_enable_sdi_bridge(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_SDI_BRIDGE_ENABLE, 0x1);
}

/**
 * drm_vid_ep_disable_sdi_bridge - Disable Video to SDI bridge
 * @drm_ep:	Pointer to DRM Vid endpoint structure
 *
 * This function enables the Video to SDI bridge.
 */
static void drm_vid_ep_disable_sdi_bridge(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_SDI_BRIDGE_ENABLE, 0x0);
}

/**
 * drm_vid_ep_enable_axi4s - Enable AXI4S-to-Video core
 * @drm_ep:	Pointer to DRM Vid endpoint structure
 *
 * This function enables the AXI4S-to-Video core.
 */
static void drm_vid_ep_enable_axi4s(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_AXI_S_VID_ENABLE, 0x1);
}

/**
 * drm_vid_ep_disable_axi4s - Disable AXI4S-to-Video core
 * @drm_ep:	Pointer to DRM Vid endpoint structure
 *
 * This function enables the AXI4S-to-Video core.
 */
static void drm_vid_ep_disable_axi4s(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_AXI_S_VID_ENABLE, 0x0);
}

/**
 * drm_vid_ep_irq_handler - DRM Video Endpoint interrupt
 * @irq:	irq number
 * @data:	irq data
 *
 * Return: IRQ_HANDLED for all cases.
 *
 * This is the DRM Video Endpoint ready interrupt.
 */
static irqreturn_t drm_vid_ep_irq_handler(int irq, void *data)
{
	struct drm_vid_ep *drm_ep = (struct drm_vid_ep *)data;
	u32 reg;
  u32 clr = 0;

	reg = drm_vid_ep_readl(drm_ep->base, RREG_IRQ_STATUS);
	if (reg & AXI_S_VID_LOCKED_INTR) {
		dev_err_ratelimited(drm_ep->dev, "AXI-4 Stream Locked\n");
    clr = clr | AXI_S_VID_LOCKED_INTR;
  }
	if (reg & AXI_S_VID_OVERFLOW_INTR) {
		dev_err_ratelimited(drm_ep->dev, "AXI-4 Stream Overflow error\n");
    clr = clr | AXI_S_VID_OVERFLOW_INTR;
  }
	if (reg & AXI_S_VID_UNDERFLOW_INTR) {
		dev_err_ratelimited(drm_ep->dev, "AXI-4 Stream Underflow error\n");
    clr = clr | AXI_S_VID_UNDERFLOW_INTR;
  }
	drm_vid_ep_writel(drm_ep->base, WREG_IRQ_CLEAR, clr);

	return IRQ_HANDLED;
}

/**
 * drm_vid_ep_set_display_disable - Disable the DRM Video Endpoint core enable
 * register bit
 * @drm_vid_ep: DRM endpoint structure having the updated user parameters
 *
 * This function takes the DRM Endpoint strucure and disables the core enable bit
 * of core configuration register.
 */
static void drm_vid_ep_set_display_disable(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_IRQ_ENABLE, 0x0);
	drm_vid_ep_writel(drm_ep->base, WREG_CORE_ENABLE, 0x0);
}

/**
 * drm_vid_ep_set_sdi_mode -  Set SDI mode parameters in DRM Video Endpoint
 * @drm_ep:	pointer DRM endpoint structure
 * @mode:	SDI display mode
 * @is_frac:	0 - integer 1 - fractional
 */
static void drm_vid_ep_set_sdi_mode(struct drm_vid_ep *drm_ep, u32 mode,
			      bool is_frac)
{
  switch(mode) {
    case SDI_MODE_SD:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x1);
      break;
    case SDI_MODE_HD:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x0);
      break;
    case SDI_MODE_3GA:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x2);
      break;
    case SDI_MODE_6G:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x4);
      break;
    case SDI_MODE_12G:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x5);
      break;
    default:
      drm_vid_ep_writel(drm_ep->base, WREG_SDI_MODE, 0x0);
      break;
  }

  if (is_frac) {
    drm_vid_ep_writel(drm_ep->base, WREG_SDI_IS_FRAC, 0x1);
  } else {
    drm_vid_ep_writel(drm_ep->base, WREG_SDI_IS_FRAC, 0x0);
  }
}

/**
 * drm_vid_ep_set_config_parameters - Configure DRM Video Endpoint registers with parameters
 * given from user application.
 * @drm_ep: DRM endpoint structure having the updated user parameters
 *
 * This function takes the DRM endpoint structure having drm_property parameters
 * configured from  user application and writes them into IP registers.
 */
static void drm_vid_ep_set_config_parameters(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_set_sdi_mode(drm_ep, drm_ep->sdi_mod_prop_val, drm_ep->is_frac_prop_val);
}

/**
 * drm_vid_ep_atomic_set_property - implementation of drm_connector_funcs
 * set_property invoked by IOCTL call to DRM_IOCTL_MODE_OBJ_SETPROPERTY
 *
 * @connector: pointer DRM Video Endpoint connector
 * @state: DRM connector state
 * @property: pointer to the drm_property structure
 * @val: parameter value that is configured from user application
 *
 * This function takes a drm_property name and value given from user application
 * and update the DRM Video EP structure property varabiles with the values.
 * These values are later used to configure the DRM Video Endpoint.
 *
 * Return: 0 on success OR -EINVAL if setting property fails
 */
static int
drm_vid_ep_atomic_set_property(struct drm_connector *connector,
			     struct drm_connector_state *state,
			     struct drm_property *property, uint64_t val)
{
	struct drm_vid_ep *drm_ep = connector_to_drm_vid_ep(connector);

	if (property == drm_ep->sdi_mode)
		drm_ep->sdi_mod_prop_val = (unsigned int)val;
	else if (property == drm_ep->sdi_data_strm)
		drm_ep->sdi_data_strm_prop_val = (unsigned int)val;
	else if (property == drm_ep->sdi_420_in)
		drm_ep->sdi_420_in_val = val;
	else if (property == drm_ep->sdi_420_out)
		drm_ep->sdi_420_out_val = val;
	else if (property == drm_ep->is_frac_prop)
		drm_ep->is_frac_prop_val = !!val;
	else if (property == drm_ep->height_out)
		drm_ep->height_out_prop_val = (unsigned int)val;
	else if (property == drm_ep->width_out)
		drm_ep->width_out_prop_val = (unsigned int)val;
	else if (property == drm_ep->in_fmt)
		drm_ep->in_fmt_prop_val = (unsigned int)val;
	else if (property == drm_ep->out_fmt)
		drm_ep->out_fmt_prop_val = (unsigned int)val;
	else
		return -EINVAL;
	return 0;
}

static int
drm_vid_ep_atomic_get_property(struct drm_connector *connector,
			     const struct drm_connector_state *state,
			     struct drm_property *property, uint64_t *val)
{
	struct drm_vid_ep *drm_ep = connector_to_drm_vid_ep(connector);

	if (property == drm_ep->sdi_mode)
		*val = drm_ep->sdi_mod_prop_val;
	else if (property == drm_ep->sdi_data_strm)
		*val =  drm_ep->sdi_data_strm_prop_val;
	else if (property == drm_ep->sdi_420_in)
		*val = drm_ep->sdi_420_in_val;
	else if (property == drm_ep->sdi_420_out)
		*val = drm_ep->sdi_420_out_val;
	else if (property == drm_ep->is_frac_prop)
		*val =  drm_ep->is_frac_prop_val;
	else if (property == drm_ep->height_out)
		*val = drm_ep->height_out_prop_val;
	else if (property == drm_ep->width_out)
		*val = drm_ep->width_out_prop_val;
	else if (property == drm_ep->in_fmt)
		*val = drm_ep->in_fmt_prop_val;
	else if (property == drm_ep->out_fmt)
		*val = drm_ep->out_fmt_prop_val;
	else
		return -EINVAL;

	return 0;
}

/**
 * drm_vid_ep_get_sdi_mode_id - Search for a video mode in the supported modes table
 *
 * @mode: mode being searched
 *
 * Return: mode id if mode is found OR -EINVAL otherwise
 */
static int drm_vid_ep_get_sdi_mode_id(struct drm_display_mode *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(xlnx_sdi_modes); i++)
		if (drm_mode_equal(&xlnx_sdi_modes[i].mode, mode))
			return i;
	return -EINVAL;
}

/**
 * drm_vid_ep_add_sdi_modes - Adds SDI supported modes
 * @connector: pointer DRM Video Endpoint connector
 *
 * Return:	Count of modes added
 *
 * This function adds the SDI modes supported and returns its count
 */
static int drm_vid_ep_add_sdi_modes(struct drm_connector *connector)
{
	int num_modes = 0;
	u32 i;
	struct drm_display_mode *mode;
	struct drm_device *dev = connector->dev;

	for (i = 0; i < ARRAY_SIZE(xlnx_sdi_modes); i++) {
		const struct drm_display_mode *ptr = &xlnx_sdi_modes[i].mode;

		mode = drm_mode_duplicate(dev, ptr);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			num_modes++;
		}
	}
	return num_modes;
}

static enum drm_connector_status
drm_vid_ep_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;
}

static void drm_vid_ep_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	connector->dev = NULL;
}

static const struct drm_connector_funcs drm_vid_ep_connector_funcs = {
	.detect = drm_vid_ep_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_vid_ep_connector_destroy,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_set_property = drm_vid_ep_atomic_set_property,
	.atomic_get_property = drm_vid_ep_atomic_get_property,
};

static struct drm_encoder *
drm_vid_ep_best_encoder(struct drm_connector *connector)
{
	return &(connector_to_drm_vid_ep(connector)->encoder);
}

static int drm_vid_ep_get_sdi_modes(struct drm_connector *connector)
{
	return drm_vid_ep_add_sdi_modes(connector);
}

static struct drm_connector_helper_funcs drm_vid_ep_connector_helper_funcs = {
	.get_modes = drm_vid_ep_get_sdi_modes,
	.best_encoder = drm_vid_ep_best_encoder,
};

/**
 * drm_vid_ep_drm_connector_create_property -  create DRM Video Endpoint connector properties
 *
 * @base_connector: pointer to DRM Video Endpoint connector
 *
 * This function takes the DRM Video Endpoint connector component and defines
 * the drm_property variables with their default values.
 */
static void
drm_vid_ep_drm_connector_create_property(struct drm_connector *base_connector)
{
	struct drm_device *dev = base_connector->dev;
	struct drm_vid_ep *drm_ep  = connector_to_drm_vid_ep(base_connector);

	drm_ep->is_frac_prop = drm_property_create_bool(dev, 0, "is_frac");
	drm_ep->sdi_mode = drm_property_create_range(dev, 0,
						  "sdi_mode", 0, 5);
	drm_ep->sdi_data_strm = drm_property_create_range(dev, 0,
						       "sdi_data_stream", 2, 8);
	drm_ep->sdi_420_in = drm_property_create_bool(dev, 0, "sdi_420_in");
	drm_ep->sdi_420_out = drm_property_create_bool(dev, 0, "sdi_420_out");
	drm_ep->height_out = drm_property_create_range(dev, 0,
						    "height_out", 2, 4096);
	drm_ep->width_out = drm_property_create_range(dev, 0,
						   "width_out", 2, 4096);
	drm_ep->in_fmt = drm_property_create_range(dev, 0,
						"in_fmt", 0, 16384);
	drm_ep->out_fmt = drm_property_create_range(dev, 0,
						 "out_fmt", 0, 16384);
}

/**
 * drm_vid_ep_drm_connector_attach_property -  attach DRM Video Endpoint connector
 * properties
 *
 * @base_connector: pointer to DRM Video Endpoint connector
 */
static void
drm_vid_ep_drm_connector_attach_property(struct drm_connector *base_connector)
{
	struct drm_vid_ep *drm_ep = connector_to_drm_vid_ep(base_connector);
	struct drm_mode_object *obj = &base_connector->base;

	if (drm_ep->sdi_mode)
		drm_object_attach_property(obj, drm_ep->sdi_mode, 0);

	if (drm_ep->sdi_data_strm)
		drm_object_attach_property(obj, drm_ep->sdi_data_strm, 0);

	if (drm_ep->sdi_420_in)
		drm_object_attach_property(obj, drm_ep->sdi_420_in, 0);

	if (drm_ep->sdi_420_out)
		drm_object_attach_property(obj, drm_ep->sdi_420_out, 0);

	if (drm_ep->is_frac_prop)
		drm_object_attach_property(obj, drm_ep->is_frac_prop, 0);

	if (drm_ep->height_out)
		drm_object_attach_property(obj, drm_ep->height_out, 0);

	if (drm_ep->width_out)
		drm_object_attach_property(obj, drm_ep->width_out, 0);

	if (drm_ep->in_fmt)
		drm_object_attach_property(obj, drm_ep->in_fmt, 0);

	if (drm_ep->out_fmt)
		drm_object_attach_property(obj, drm_ep->out_fmt, 0);
}

static int drm_vid_ep_create_connector(struct drm_encoder *encoder)
{
	struct drm_vid_ep *drm_ep = encoder_to_drm_vid_ep(encoder);
	struct drm_connector *connector = &drm_ep->connector;
	int ret;

	connector->interlace_allowed = true;
	connector->doublescan_allowed = true;

	ret = drm_connector_init(encoder->dev, connector,
				 &drm_vid_ep_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret) {
		dev_err(drm_ep->dev, "Failed to initialize connector with drm\n");
		return ret;
	}

	drm_connector_helper_add(connector, &drm_vid_ep_connector_helper_funcs);
	drm_connector_register(connector);
	drm_connector_attach_encoder(connector, encoder);
	drm_vid_ep_drm_connector_create_property(connector);
	drm_vid_ep_drm_connector_attach_property(connector);

	return 0;
}

/**
 * drm_vid_ep_set_display_enable - Enables the DRM Video Endpoint core enable
 * register bit
 * @drm_ep: DRM Video Endpoint structure having the updated user parameters
 *
 * This function takes the DRM Video Endpoint strucure and enables the core enable bit
 * of core configuration register.
 */
static void drm_vid_ep_set_display_enable(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_CORE_ENABLE, 0x1);
}

static void drm_vid_ep_setup(struct drm_vid_ep *drm_ep)
{
	drm_vid_ep_writel(drm_ep->base, WREG_IRQ_MASK, IRQ_EN_MASK);
	drm_vid_ep_writel(drm_ep->base, WREG_IRQ_ENABLE, 0x1);
	xlnx_stc_reset(drm_ep->base);
}

/**
 * drm_vid_ep_encoder_atomic_mode_set -  drive the SDI timing parameters
 *
 * @encoder: pointer to DRM encoder
 * @crtc_state: DRM crtc state
 * @connector_state: DRM connector state
 *
 * This function derives the SDI IP timing parameters from the timing
 * values given to timing module.
 */
static void drm_vid_ep_encoder_atomic_mode_set(struct drm_encoder *encoder,
					     struct drm_crtc_state *crtc_state,
				  struct drm_connector_state *connector_state)
{
	struct drm_vid_ep *drm_ep = encoder_to_drm_vid_ep(encoder);
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	struct videomode vm;
	u32 i;
	u32 sditx_blank, vtc_blank;

	/* Set timing parameters as per bridge output parameters */
	xlnx_bridge_set_input(drm_ep->bridge, adjusted_mode->hdisplay,
			      adjusted_mode->vdisplay, drm_ep->in_fmt_prop_val);
	xlnx_bridge_set_output(drm_ep->bridge, drm_ep->width_out_prop_val,
			       drm_ep->height_out_prop_val, drm_ep->out_fmt_prop_val);
	xlnx_bridge_enable(drm_ep->bridge);

	if (drm_ep->bridge) {
		for (i = 0; i < ARRAY_SIZE(xlnx_sdi_modes); i++) {
			if (xlnx_sdi_modes[i].mode.hdisplay ==
			    drm_ep->width_out_prop_val &&
			    xlnx_sdi_modes[i].mode.vdisplay ==
			    drm_ep->height_out_prop_val &&
			    xlnx_sdi_modes[i].mode.vrefresh ==
			    adjusted_mode->vrefresh) {
				memcpy((char *)adjusted_mode +
				       offsetof(struct drm_display_mode,
						clock),
				       &xlnx_sdi_modes[i].mode.clock,
				       SDI_TIMING_PARAMS_SIZE);
				break;
			}
		}
	}

	drm_vid_ep_setup(drm_ep);
	drm_vid_ep_set_config_parameters(drm_ep);

	/* UHDSDI is fixed 2 pixels per clock, horizontal timings div by 2 */
	vm.hactive = adjusted_mode->hdisplay / PIXELS_PER_CLK;
	vm.hfront_porch = (adjusted_mode->hsync_start -
			  adjusted_mode->hdisplay) / PIXELS_PER_CLK;
	vm.hback_porch = (adjusted_mode->htotal -
			 adjusted_mode->hsync_end) / PIXELS_PER_CLK;
	vm.hsync_len = (adjusted_mode->hsync_end -
		       adjusted_mode->hsync_start) / PIXELS_PER_CLK;

	vm.vactive = adjusted_mode->vdisplay;
	vm.vfront_porch = adjusted_mode->vsync_start -
			  adjusted_mode->vdisplay;
	if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE) {
		/* vtotal records total size for full frame, not for field */
		if (adjusted_mode->vtotal == 1125)
			vm.vback_porch = 562 - adjusted_mode->vsync_end;
		else if (adjusted_mode->vtotal == 625)
			vm.vback_porch = 312 - adjusted_mode->vsync_end;
		else if (adjusted_mode->vtotal == 525)
			vm.vback_porch = 262 - adjusted_mode->vsync_end;
	}
	else {
		vm.vback_porch = adjusted_mode->vtotal -
			         adjusted_mode->vsync_end;
	}

	vm.vsync_len = adjusted_mode->vsync_end -
		       adjusted_mode->vsync_start;
	vm.flags = 0;
	if (adjusted_mode->flags & DRM_MODE_FLAG_INTERLACE)
		vm.flags |= DISPLAY_FLAGS_INTERLACED;
	if (adjusted_mode->flags & DRM_MODE_FLAG_PHSYNC)
		vm.flags |= DISPLAY_FLAGS_HSYNC_LOW;
	if (adjusted_mode->flags & DRM_MODE_FLAG_PVSYNC)
		vm.flags |= DISPLAY_FLAGS_VSYNC_LOW;

	do {
		sditx_blank = (adjusted_mode->hsync_start -
			       adjusted_mode->hdisplay) +
			      (adjusted_mode->hsync_end -
			       adjusted_mode->hsync_start) +
			      (adjusted_mode->htotal -
			       adjusted_mode->hsync_end);

		vtc_blank = (vm.hfront_porch + vm.hback_porch +
			     vm.hsync_len) * PIXELS_PER_CLK;

		if (vtc_blank != sditx_blank)
			vm.hfront_porch++;
	} while (vtc_blank < sditx_blank);

	vm.pixelclock = adjusted_mode->clock * 1000;

	/* parameters for sdi audio */
	drm_ep->video_mode.vdisplay = adjusted_mode->vdisplay;
	drm_ep->video_mode.hdisplay = adjusted_mode->hdisplay;
	drm_ep->video_mode.vrefresh = adjusted_mode->vrefresh;
	drm_ep->video_mode.flags = adjusted_mode->flags;

	xlnx_stc_sig(drm_ep->base, &vm);
}

static void drm_vid_ep_commit(struct drm_encoder *encoder)
{
	struct drm_vid_ep *drm_ep = encoder_to_drm_vid_ep(encoder);

	dev_dbg(drm_ep->dev, "%s\n", __func__);
	drm_vid_ep_set_display_enable(drm_ep);
	/* enable sdi bridge, timing controller and Axi4s_vid_out_ctrl */
  drm_vid_ep_enable_sdi_bridge(drm_ep);
	xlnx_stc_enable(drm_ep->base);
  xlnx_stc_fsync_enable(drm_ep->base);
	drm_vid_ep_enable_axi4s(drm_ep);
}

static void drm_vid_ep_disable(struct drm_encoder *encoder)
{
	struct drm_vid_ep *drm_ep = encoder_to_drm_vid_ep(encoder);

	if (drm_ep->bridge)
		xlnx_bridge_disable(drm_ep->bridge);

	drm_vid_ep_set_display_disable(drm_ep);
  drm_vid_ep_disable_axi4s(drm_ep);
  drm_vid_ep_disable_sdi_bridge(drm_ep);
  xlnx_stc_fsync_disable(drm_ep->base);
	xlnx_stc_disable(drm_ep->base);
}

static const struct drm_encoder_helper_funcs drm_vid_ep_encoder_helper_funcs = {
	.atomic_mode_set	= drm_vid_ep_encoder_atomic_mode_set,
	.enable			= drm_vid_ep_commit,
	.disable		= drm_vid_ep_disable,
};

static const struct drm_encoder_funcs drm_vid_ep_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int drm_vid_ep_bind(struct device *dev, struct device *master,
			 void *data)
{
	struct drm_vid_ep *drm_ep = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &drm_ep->encoder;
	struct drm_device *drm_dev = data;
	int ret;

	/*
	 * TODO: The possible CRTCs are 1 now as per current implementation of
	 * SDI tx drivers. DRM framework can support more than one CRTCs and
	 * SDI driver can be enhanced for that.
	 */
	encoder->possible_crtcs = 1;

	drm_encoder_init(drm_dev, encoder, &drm_vid_ep_encoder_funcs,
			 DRM_MODE_ENCODER_TMDS, NULL);

	drm_encoder_helper_add(encoder, &drm_vid_ep_encoder_helper_funcs);

	ret = drm_vid_ep_create_connector(encoder);
	if (ret) {
		dev_err(drm_ep->dev, "fail creating connector, ret = %d\n", ret);
		drm_encoder_cleanup(encoder);
	}
	return ret;
}

static void drm_vid_ep_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct drm_vid_ep *drm_ep = dev_get_drvdata(dev);

	drm_vid_ep_set_display_disable(drm_ep);
  drm_vid_ep_disable_axi4s(drm_ep);
  drm_vid_ep_disable_sdi_bridge(drm_ep);
  xlnx_stc_fsync_disable(drm_ep->base);
	xlnx_stc_disable(drm_ep->base);
	drm_encoder_cleanup(&drm_ep->encoder);
	drm_connector_cleanup(&drm_ep->connector);
	xlnx_bridge_disable(drm_ep->bridge);
}

static const struct component_ops drm_vid_ep_component_ops = {
	.bind	= drm_vid_ep_bind,
	.unbind	= drm_vid_ep_unbind,
};

static int drm_vid_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct drm_vid_ep *drm_ep;
	struct device_node *vpss_node;
	int ret, irq;
	struct device_node *ports, *port;
	u32 nports = 0, portmask = 0;

	drm_ep = devm_kzalloc(dev, sizeof(*drm_ep), GFP_KERNEL);
	if (!drm_ep)
		return -ENOMEM;

	drm_ep->dev = dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	drm_ep->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(drm_ep->base)) {
		dev_err(dev, "failed to remap io region\n");
		return PTR_ERR(drm_ep->base);
	}
	platform_set_drvdata(pdev, drm_ep);

	drm_ep->axi_clk = devm_clk_get(dev, "s_axi_aclk");
	if (IS_ERR(drm_ep->axi_clk)) {
		ret = PTR_ERR(drm_ep->axi_clk);
		dev_err(dev, "failed to get s_axi_aclk %d\n", ret);
		return ret;
	}

	drm_ep->vidin_clk = devm_clk_get(dev, "video_in_clk");
	if (IS_ERR(drm_ep->vidin_clk)) {
		ret = PTR_ERR(drm_ep->vidin_clk);
		dev_err(dev, "failed to get video_in_clk %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(drm_ep->axi_clk);
	if (ret) {
		dev_err(dev, "failed to enable axi_clk %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(drm_ep->vidin_clk);
	if (ret) {
		dev_err(dev, "failed to enable vidin_clk %d\n", ret);
	}

	/* in case all "port" nodes are grouped under a "ports" node */
	ports = of_get_child_by_name(drm_ep->dev->of_node, "ports");
	if (!ports) {
		dev_dbg(dev, "Searching for port nodes in device node.\n");
		ports = drm_ep->dev->of_node;
	}

	for_each_child_of_node(ports, port) {
		struct device_node *endpoint;
		u32 index;

		if (!port->name || of_node_cmp(port->name, "port")) {
			dev_dbg(dev, "port name is null or node name is not port!\n");
			continue;
		}

		endpoint = of_get_next_child(port, NULL);
		if (!endpoint) {
			dev_err(dev, "No remote port at %s\n", port->name);
			of_node_put(endpoint);
			ret = -EINVAL;
			goto err_disable_vidin_clk;
		}

		of_node_put(endpoint);

		ret = of_property_read_u32(port, "reg", &index);
		if (ret) {
			dev_err(dev, "reg property not present - %d\n", ret);
			goto err_disable_vidin_clk;
		}

		portmask |= (1 << index);

		nports++;
	}

	/* disable interrupt */
	drm_vid_ep_writel(drm_ep->base, WREG_IRQ_ENABLE, 0x0);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_disable_vidin_clk;
	}

	ret = devm_request_threaded_irq(drm_ep->dev, irq, NULL,
					drm_vid_ep_irq_handler, IRQF_ONESHOT,
					dev_name(drm_ep->dev), drm_ep);
	if (ret < 0)
		goto err_disable_vidin_clk;

	/* Bridge support */
	vpss_node = of_parse_phandle(drm_ep->dev->of_node, "xlnx,vpss", 0);
	if (vpss_node) {
		drm_ep->bridge = of_xlnx_bridge_get(vpss_node);
		if (!drm_ep->bridge) {
			dev_info(drm_ep->dev, "Didn't get bridge instance\n");
			ret = -EPROBE_DEFER;
			goto err_disable_vidin_clk;
		}
	}

	/* video mode properties needed by audio driver are shared to audio
	 * driver through a pointer in platform data. This will be used in
	 * audio driver. The solution may be needed to modify/extend to avoid
	 * probable error scenarios
	 */
	pdev->dev.platform_data = &drm_ep->video_mode;

	ret = component_add(dev, &drm_vid_ep_component_ops);
	if (ret < 0)
		goto err_disable_vidin_clk;

	return ret;

err_disable_vidin_clk:
	clk_disable_unprepare(drm_ep->vidin_clk);
err_disable_axi_clk:
	clk_disable_unprepare(drm_ep->axi_clk);

	return ret;
}

static int drm_vid_ep_remove(struct platform_device *pdev)
{
	struct drm_vid_ep *drm_ep = platform_get_drvdata(pdev);

	component_del(&pdev->dev, &drm_vid_ep_component_ops);
	clk_disable_unprepare(drm_ep->vidin_clk);
	clk_disable_unprepare(drm_ep->axi_clk);

	return 0;
}

static const struct of_device_id drm_vid_ep_of_match[] = {
	{ .compatible = "evertz,drm-vid-ep"},
	{ }
};
MODULE_DEVICE_TABLE(of, drm_vid_ep_of_match);

static struct platform_driver drm_vid_ep_driver = {
	.probe = drm_vid_ep_probe,
	.remove = drm_vid_ep_remove,
	.driver = {
		.name = "drm-vid-ep",
		.of_match_table = drm_vid_ep_of_match,
	},
};

module_platform_driver(drm_vid_ep_driver);

MODULE_DESCRIPTION("DRM Video Endpoint Driver");
MODULE_LICENSE("GPL v2");
