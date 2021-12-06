/*
 * Copyright (c) 2020 PHYTEC Messtechnik GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT gooddisplay_jd79653

#include <string.h>
#include <device.h>
#include <init.h>
#include <drivers/display.h>
#include <drivers/gpio.h>
#include <drivers/spi.h>
#include <sys/byteorder.h>

#include "gd79653_regs.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(jd79653, CONFIG_DISPLAY_LOG_LEVEL);

/**
 * JD79653 compatible EPD controller driver.
 *
 * Currently only the black/white pannels are supported (KW mode),
 * also first gate/source should be 0.
 */

#define JD79653_SPI_FREQ DT_INST_PROP(0, spi_max_frequency)
#define JD79653_BUS_NAME DT_INST_BUS_LABEL(0)
#define JD79653_DC_PIN DT_INST_GPIO_PIN(0, dc_gpios)
#define JD79653_DC_FLAGS DT_INST_GPIO_FLAGS(0, dc_gpios)
#define JD79653_DC_CNTRL DT_INST_GPIO_LABEL(0, dc_gpios)
#define JD79653_CS_PIN DT_INST_SPI_DEV_CS_GPIOS_PIN(0)
#define JD79653_CS_FLAGS DT_INST_SPI_DEV_CS_GPIOS_FLAGS(0)
#if DT_INST_SPI_DEV_HAS_CS_GPIOS(0)
#define JD79653_CS_CNTRL DT_INST_SPI_DEV_CS_GPIOS_LABEL(0)
#endif
#define JD79653_BUSY_PIN DT_INST_GPIO_PIN(0, busy_gpios)
#define JD79653_BUSY_CNTRL DT_INST_GPIO_LABEL(0, busy_gpios)
#define JD79653_BUSY_FLAGS DT_INST_GPIO_FLAGS(0, busy_gpios)
#define JD79653_RESET_PIN DT_INST_GPIO_PIN(0, reset_gpios)
#define JD79653_RESET_CNTRL DT_INST_GPIO_LABEL(0, reset_gpios)
#define JD79653_RESET_FLAGS DT_INST_GPIO_FLAGS(0, reset_gpios)

#define EPD_PANEL_WIDTH			DT_INST_PROP(0, width)
#define EPD_PANEL_HEIGHT		DT_INST_PROP(0, height)
#define JD79653_PIXELS_PER_BYTE		8U

/* Horizontally aligned page! */
#define JD79653_NUMOF_PAGES		(EPD_PANEL_WIDTH / \
					 JD79653_PIXELS_PER_BYTE)
#define JD79653_PANEL_FIRST_GATE		0U
#define JD79653_PANEL_LAST_GATE		(EPD_PANEL_HEIGHT - 1)
#define JD79653_PANEL_FIRST_PAGE		0U
#define JD79653_PANEL_LAST_PAGE		(JD79653_NUMOF_PAGES - 1)
#define JD79653_BUFFER_SIZE (EPD_PANEL_HEIGHT * JD79653_NUMOF_PAGES)

struct gd79653_data {
	const struct device *reset;
	const struct device *dc;
	const struct device *busy;
	const struct device *spi_dev;
	struct spi_config spi_config;
#if defined(JD79653_CS_CNTRL)
	struct spi_cs_control cs_ctrl;
#endif
};

static uint8_t gd79653_softstart[] = DT_INST_PROP(0, softstart);
static uint8_t gd79653_pwr[] = DT_INST_PROP(0, pwr);

static uint8_t old_buffer[JD79653_BUFFER_SIZE];
static bool blanking_on = true;

static inline int gd79653_write_cmd(struct gd79653_data *driver,
				   uint8_t cmd, uint8_t *data, size_t len)
{
	struct spi_buf buf = {.buf = &cmd, .len = sizeof(cmd)};
	struct spi_buf_set buf_set = {.buffers = &buf, .count = 1};

	gpio_pin_set(driver->dc, JD79653_DC_PIN, 1);
	if (spi_write(driver->spi_dev, &driver->spi_config, &buf_set)) {
		return -EIO;
	}

	if (data != NULL) {
		buf.buf = data;
		buf.len = len;
		gpio_pin_set(driver->dc, JD79653_DC_PIN, 0);
		if (spi_write(driver->spi_dev, &driver->spi_config, &buf_set)) {
			return -EIO;
		}
	}

	return 0;
}

static inline void gd79653_busy_wait(struct gd79653_data *driver)
{
	int pin = gpio_pin_get(driver->busy, JD79653_BUSY_PIN);

	while (pin > 0) {
		__ASSERT(pin >= 0, "Failed to get pin level");
		LOG_DBG("wait %u", pin);
		k_sleep(K_MSEC(JD79653_BUSY_DELAY));
		pin = gpio_pin_get(driver->busy, JD79653_BUSY_PIN);
	}
}


static int gd79653_update_display(const struct device *dev)
{
	struct gd79653_data *driver = dev->data;

	LOG_DBG("Trigger update sequence");
	if (gd79653_write_cmd(driver, JD79653_CMD_DRF, NULL, 0)) {
		return -EIO;
	}

	k_sleep(K_MSEC(JD79653_BUSY_DELAY));

	return 0;
}

static int gd79653_blanking_off(const struct device *dev)
{
	struct gd79653_data *driver = dev->data;

	if (blanking_on) {
		/* Update EPD pannel in normal mode */
		gd79653_busy_wait(driver);
		if (gd79653_update_display(dev)) {
			return -EIO;
		}
	}

	blanking_on = false;

	return 0;
}

static int gd79653_blanking_on(const struct device *dev)
{
	blanking_on = true;

	return 0;
}

static int gd79653_write(const struct device *dev, const uint16_t x, const uint16_t y,
			const struct display_buffer_descriptor *desc,
			const void *buf)
{
	struct gd79653_data *driver = dev->data;
	uint16_t x_end_idx = x + desc->width - 1;
	uint16_t y_end_idx = y + desc->height - 1;
	uint8_t ptl[JD79653_PTL_REG_LENGTH] = {0};
	size_t buf_len;

	LOG_DBG("x %u, y %u, height %u, width %u, pitch %u",
		x, y, desc->height, desc->width, desc->pitch);

	buf_len = MIN(desc->buf_size,
		      desc->height * desc->width / JD79653_PIXELS_PER_BYTE);
	__ASSERT(desc->width <= desc->pitch, "Pitch is smaller then width");
	__ASSERT(buf != NULL, "Buffer is not available");
	__ASSERT(buf_len != 0U, "Buffer of length zero");
	__ASSERT(!(desc->width % JD79653_PIXELS_PER_BYTE),
		 "Buffer width not multiple of %d", JD79653_PIXELS_PER_BYTE);

	if ((y_end_idx > (EPD_PANEL_HEIGHT - 1)) ||
	    (x_end_idx > (EPD_PANEL_WIDTH - 1))) {
		LOG_ERR("Position out of bounds");
		return -EINVAL;
	}

	/* Setup Partial Window and enable Partial Mode */
	ptl[JD79653_PTL_HRST_IDX]  = x;
	ptl[JD79653_PTL_HRED_IDX]  = x_end_idx;
    ptl[JD79653_PTL_HRESERVED] = 0x00;
	ptl[JD79653_PTL_VRST_IDX]  = y;
    ptl[JD79653_PTL_VRESERVED] = 0x00;
	ptl[JD79653_PTL_VRED_IDX]  = y_end_idx;
    
	ptl[sizeof(ptl) - 1] = JD79653_PTL_PT_SCAN;
	LOG_HEXDUMP_DBG(ptl, sizeof(ptl), "ptl");

	gd79653_busy_wait(driver);
	if (gd79653_write_cmd(driver, JD79653_CMD_PTIN, NULL, 0)) {
		return -EIO;
	}

	if (gd79653_write_cmd(driver, JD79653_CMD_PTL, ptl, sizeof(ptl))) {
		return -EIO;
	}

	if (gd79653_write_cmd(driver, JD79653_CMD_DTM1, old_buffer, JD79653_BUFFER_SIZE)) {
		return -EIO;
	}

	if (gd79653_write_cmd(driver, JD79653_CMD_DTM2, (uint8_t *)buf, buf_len)) {
		return -EIO;
	}

    memcpy(old_buffer, (uint8_t *)buf,  JD79653_BUFFER_SIZE);

	/* Update partial window and disable Partial Mode */
	if (blanking_on == false) {
		if (gd79653_update_display(dev)) {
			return -EIO;
		}
	}

	if (gd79653_write_cmd(driver, JD79653_CMD_PTOUT, NULL, 0)) {
		return -EIO;
	}

	return 0;
}

static int gd79653_read(const struct device *dev, const uint16_t x, const uint16_t y,
		       const struct display_buffer_descriptor *desc, void *buf)
{
	LOG_ERR("not supported");
	return -ENOTSUP;
}

static void *gd79653_get_framebuffer(const struct device *dev)
{
	LOG_ERR("not supported");
	return NULL;
}

static int gd79653_set_brightness(const struct device *dev,
				 const uint8_t brightness)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static int gd79653_set_contrast(const struct device *dev, uint8_t contrast)
{
	LOG_WRN("not supported");
	return -ENOTSUP;
}

static void gd79653_get_capabilities(const struct device *dev,
				    struct display_capabilities *caps)
{
	memset(caps, 0, sizeof(struct display_capabilities));
	caps->x_resolution = EPD_PANEL_WIDTH;
	caps->y_resolution = EPD_PANEL_HEIGHT;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO10;
	caps->current_pixel_format = PIXEL_FORMAT_MONO10;
	caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST | SCREEN_INFO_EPD;
}

static int gd79653_set_orientation(const struct device *dev,
				  const enum display_orientation
				  orientation)
{
	LOG_ERR("Unsupported");
	return -ENOTSUP;
}

static int gd79653_set_pixel_format(const struct device *dev,
				   const enum display_pixel_format pf)
{
	if (pf == PIXEL_FORMAT_MONO10) {
		return 0;
	}

	LOG_ERR("not supported");
	return -ENOTSUP;
}

static int gd79653_clear_and_write_buffer(const struct device *dev,
					 uint8_t pattern, bool update)
{
	struct display_buffer_descriptor desc = {
		.buf_size = JD79653_NUMOF_PAGES,
		.width = EPD_PANEL_WIDTH,
		.height = 1,
		.pitch = EPD_PANEL_WIDTH,
	};
	uint8_t *line;

	line = k_malloc(JD79653_NUMOF_PAGES);
	if (line == NULL) {
		return -ENOMEM;
	}

	memset(line, pattern, JD79653_NUMOF_PAGES);
	for (int i = 0; i < EPD_PANEL_HEIGHT; i++) {
		gd79653_write(dev, 0, i, &desc, line);
	}

	k_free(line);

	if (update == true) {
		if (gd79653_update_display(dev)) {
			return -EIO;
		}
	}

	return 0;
}

static int gd79653_controller_init(const struct device *dev)
{
	struct gd79653_data *driver = dev->data;
	uint8_t tmp[JD79653_PTL_REG_LENGTH];

	gpio_pin_set(driver->reset, JD79653_RESET_PIN, 1);
	k_sleep(K_MSEC(JD79653_RESET_DELAY));
	gpio_pin_set(driver->reset, JD79653_RESET_PIN, 0);
	k_sleep(K_MSEC(JD79653_RESET_DELAY));
	gd79653_busy_wait(driver);

	LOG_DBG("Initialize JD79653 controller");
    /* set panel settings - 200x200 res, booster on, BW mode, temp sense */
    tmp[0] = 0xdf;
    tmp[1] = 0x0e;
	if (gd79653_write_cmd(driver, JD79653_CMD_PSR, tmp,
			     2)) {
		return -EIO;
	}
    
    /* power settings */
	if (gd79653_write_cmd(driver, JD79653_CMD_PWR, gd79653_pwr,
			     sizeof(gd79653_pwr))) {
		return -EIO;
	}
    
    /* some fiti internal code, not in the datasheet. Good Display
     * does use this in init and some other drivers also, so here it is */
    tmp[0] = 0x55;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_4D,tmp, 1)) {
		return -EIO;
	}

    tmp[0] = 0x0f;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_AA,tmp, 1)) {
		return -EIO;
	}
    
    tmp[0] = 0x02;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_E9,tmp, 1)) {
		return -EIO;
	}

    tmp[0] = 0x11;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_B6,tmp, 1)) {
		return -EIO;
	}

    tmp[0] = 0x0a;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_F3,tmp, 1)) {
		return -EIO;
	}
    
    /* resolution settings */
    tmp[JD79653_TRES_HRES_IDX] = EPD_PANEL_WIDTH;
    tmp[1] = 0x00;
    tmp[JD79653_TRES_VRES_IDX] = EPD_PANEL_HEIGHT;
	if (gd79653_write_cmd(driver, JD79653_CMD_FIFIINT_F3,
                tmp, JD79653_TRES_REG_LENGTH)) {
		return -EIO;
	}

    /* tcon settings */
	tmp[0] = DT_INST_PROP(0, tcon);
	if (gd79653_write_cmd(driver, JD79653_CMD_TCON, tmp, 1)) {
		return -EIO;
	}

    /* vcom DC settings */
	tmp[0] = DT_INST_PROP(0, vcom);
	if (gd79653_write_cmd(driver, JD79653_CMD_VDCS, tmp, 1)) {
		return -EIO;
	}

	/* Enable Auto Sequence */
	tmp[0] = JD79653_AUTO_PON_DRF_POF;
	if (gd79653_write_cmd(driver, JD79653_CMD_AUTO, tmp, 1)) {
		return -EIO;
	}

	if (gd79653_clear_and_write_buffer(dev, 0xff, false)) {
		return -1;
	}

	return 0;
}

static int gd79653_init(const struct device *dev)
{
	struct gd79653_data *driver = dev->data;

	LOG_DBG("");

	driver->spi_dev = device_get_binding(JD79653_BUS_NAME);
	if (driver->spi_dev == NULL) {
		LOG_ERR("Could not get SPI device for JD79653");
		return -EIO;
	}

	driver->spi_config.frequency = JD79653_SPI_FREQ;
	driver->spi_config.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8);
	driver->spi_config.slave = DT_INST_REG_ADDR(0);
	driver->spi_config.cs = NULL;

	driver->reset = device_get_binding(JD79653_RESET_CNTRL);
	if (driver->reset == NULL) {
		LOG_ERR("Could not get GPIO port for JD79653 reset");
		return -EIO;
	}

	gpio_pin_configure(driver->reset, JD79653_RESET_PIN,
			   GPIO_OUTPUT_INACTIVE | JD79653_RESET_FLAGS);

	driver->dc = device_get_binding(JD79653_DC_CNTRL);
	if (driver->dc == NULL) {
		LOG_ERR("Could not get GPIO port for JD79653 DC signal");
		return -EIO;
	}

	gpio_pin_configure(driver->dc, JD79653_DC_PIN,
			   GPIO_OUTPUT_INACTIVE | JD79653_DC_FLAGS);

	driver->busy = device_get_binding(JD79653_BUSY_CNTRL);
	if (driver->busy == NULL) {
		LOG_ERR("Could not get GPIO port for JD79653 busy signal");
		return -EIO;
	}

	gpio_pin_configure(driver->busy, JD79653_BUSY_PIN,
			   GPIO_INPUT | JD79653_BUSY_FLAGS);

#if defined(JD79653_CS_CNTRL)
	driver->cs_ctrl.gpio_dev = device_get_binding(JD79653_CS_CNTRL);
	if (!driver->cs_ctrl.gpio_dev) {
		LOG_ERR("Unable to get SPI GPIO CS device");
		return -EIO;
	}

	driver->cs_ctrl.gpio_pin = JD79653_CS_PIN;
	driver->cs_ctrl.gpio_dt_flags = JD79653_CS_FLAGS;
	driver->cs_ctrl.delay = 0U;
	driver->spi_config.cs = &driver->cs_ctrl;
#endif

	return gd79653_controller_init(dev);
}

static struct gd79653_data gd79653_driver;

static struct display_driver_api gd79653_driver_api = {
	.blanking_on = gd79653_blanking_on,
	.blanking_off = gd79653_blanking_off,
	.write = gd79653_write,
	.read = gd79653_read,
	.get_framebuffer = gd79653_get_framebuffer,
	.set_brightness = gd79653_set_brightness,
	.set_contrast = gd79653_set_contrast,
	.get_capabilities = gd79653_get_capabilities,
	.set_pixel_format = gd79653_set_pixel_format,
	.set_orientation = gd79653_set_orientation,
};


DEVICE_DT_INST_DEFINE(0, gd79653_init, device_pm_control_nop,
		    &gd79653_driver, NULL,
		    POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY,
		    &gd79653_driver_api);