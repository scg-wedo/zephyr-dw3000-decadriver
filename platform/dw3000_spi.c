/*
 * Copyright 2015 (c) DecaWave Ltd, Dublin, Ireland.
 * Copyright 2019 (c) Frederic Mes, RTLOC.
 * Copyright 2021 (c) Callender-Consulting LLC.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/pinctrl.h>

#include <soc.h>
#include <nrfx_spim.h>

#include "dw3000_spi.h"

#include "version.h"

/* This file implements the SPI functions required by decadriver */

LOG_MODULE_DECLARE(dw3000, CONFIG_DW3000_LOG_LEVEL);

#define TX_WAIT_RESP_NRF52840_DELAY 30

#define DW_INST DT_INST(0, decawave_dw3000)
#define DW_SPI	DT_PARENT(DT_INST(0, decawave_dw3000))

#if KERNEL_VERSION_MAJOR > 3                                                   \
	|| (KERNEL_VERSION_MAJOR == 3 && KERNEL_VERSION_MINOR >= 4)
static struct spi_cs_control cs_ctrl = SPI_CS_CONTROL_INIT(DW_INST, 0);
#else
static struct spi_cs_control* cs_ctrl = SPI_CS_CONTROL_PTR_DT(DW_INST, 0);
#endif

#define SPI_BUS_ADDR DT_REG_ADDR(DW_SPI)

#if (SPI_BUS_ADDR == NRF_SPIM0_BASE)
	#define SPI_INSTANCE_NUM 0
#elif (SPI_BUS_ADDR == NRF_SPIM1_BASE)
	#define SPI_INSTANCE_NUM 1
#elif (SPI_BUS_ADDR == NRF_SPIM2_BASE)
	#define SPI_INSTANCE_NUM 2
#elif (SPI_BUS_ADDR == NRF_SPIM3_BASE)
	#define SPI_INSTANCE_NUM 3
#else
	#error "Unsupported SPIM instance address"
#endif

#define SPIM_INST_IDX SPI_INSTANCE_NUM
#define SPIM_INST NRFX_CONCAT_2(NRF_SPIM, SPIM_INST_IDX)
#define SPIM_INST_HANDLER NRFX_CONCAT_3(nrfx_spim_, SPIM_INST_IDX, _irq_handler)

#define SPIM_NODE DT_NODELABEL(NRFX_CONCAT_2(spi, SPIM_INST_IDX))
PINCTRL_DT_DEFINE(SPIM_NODE);

static nrfx_spim_t spim = NRFX_SPIM_INSTANCE(SPIM_INST_IDX);
static uint32_t max_spi_frequency = DT_PROP(DW_INST, spi_max_frequency);
static bool spim_initialized;
static volatile bool transfer_finished = false;

static uint8_t idatabuf[255] = {0};
static uint8_t itempbuf[255] = {0};

static void spim_handler(const nrfx_spim_evt_t *p_event, void *p_context)
{
	if (p_event->type == NRFX_SPIM_EVENT_DONE) {
		transfer_finished = true;
	}
}

int dw3000_spi_init(void)
{
#if defined(__ZEPHYR__)
	IRQ_DIRECT_CONNECT(NRFX_IRQ_NUMBER_GET(NRF_SPIM_INST_GET(SPIM_INST_IDX)), IRQ_PRIO_LOWEST,
				NRFX_SPIM_INST_HANDLER_GET(SPIM_INST_IDX), 0);
#endif

	dw3000_spi_speed_slow();

	// initialized correctly at boot but after fini we need to reconfigure
	gpio_pin_configure_dt(&cs_ctrl.gpio, GPIO_OUTPUT_HIGH);

	return 0;
}

void dw3000_spi_speed_slow(void)
{
	int err;

	if (spim_initialized) {
		nrfx_spim_uninit(&spim);
		spim_initialized = false;
	}

	nrfx_spim_config_t spim_config = NRFX_SPIM_DEFAULT_CONFIG(
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED
	);
	spim_config.frequency = NRFX_MHZ_TO_HZ(2);
	spim_config.skip_gpio_cfg = true;
	spim_config.skip_psel_cfg = true;

	err = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPIM_NODE),
							  PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		LOG_INF("pinctrl_apply_state() slow speed failed: 0x%08x", err);
		return;
	}

	err = nrfx_spim_init(&spim, &spim_config, spim_handler, NULL);
	if (err != NRFX_SUCCESS) {
		LOG_INF("nrfx_spim_init() slow speed failed: 0x%08x", err);
		return;
	}

	spim_initialized = true;
}

void dw3000_spi_speed_fast(void)
{
	int err;

	if (spim_initialized) {
		nrfx_spim_uninit(&spim);
		spim_initialized = false;
	}

	nrfx_spim_config_t spim_config = NRFX_SPIM_DEFAULT_CONFIG(
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED,
		NRF_SPIM_PIN_NOT_CONNECTED
	);
	spim_config.frequency = max_spi_frequency;
	spim_config.skip_gpio_cfg = true;
	spim_config.skip_psel_cfg = true;

	err = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SPIM_NODE),
							  PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		LOG_INF("pinctrl_apply_state() fast speed failed: 0x%08x", err);
		return;
	}

	err = nrfx_spim_init(&spim, &spim_config, spim_handler, NULL);
	if (err != NRFX_SUCCESS) {
		LOG_INF("nrfx_spim_init() fast speed failed: 0x%08x", err);
		return;
	}

	spim_initialized = true;
}

void dw3000_spi_fini(void)
{
	if (spim_initialized) {
		nrfx_spim_uninit(&spim);
		spim_initialized = false;
	}

	gpio_pin_configure_dt(&cs_ctrl.gpio, GPIO_DISCONNECTED);
}

int32_t dw3000_spi_write_crc(uint16_t headerLength, const uint8_t* headerBuffer,
							 uint16_t bodyLength, const uint8_t* bodyBuffer,
							 uint8_t crc8)
{
	uint8_t *p1;
	uint32_t idatalength = headerLength + bodyLength + sizeof(crc8);

	p1 = idatabuf;
	memcpy(p1, headerBuffer, headerLength);
	p1 += headerLength;
	memcpy(p1, bodyBuffer, bodyLength);
	p1 += bodyLength;
	memcpy(p1, &crc8, 1);

	nrfx_err_t err;
	nrfx_spim_xfer_desc_t xfer_desc = {
		.p_tx_buffer = idatabuf,
		.tx_length = idatalength,
		.p_rx_buffer = itempbuf,
		.rx_length = idatalength,
	};

	transfer_finished = false;
	gpio_pin_set_dt(&cs_ctrl.gpio, 1);
	err = nrfx_spim_xfer(&spim, &xfer_desc, 0);
	if (err != NRFX_SUCCESS) {
		LOG_DBG("nrfx_spim_xfer() failed: 0x%08x", err);
		return -EIO;
	}
	while (!transfer_finished);
	gpio_pin_set_dt(&cs_ctrl.gpio, 0);

	return 0;
}

int32_t dw3000_spi_write(uint16_t headerLength, const uint8_t* headerBuffer,
						 uint16_t bodyLength, const uint8_t* bodyBuffer)
{
	uint8_t *p1;
	uint32_t idatalength = headerLength + bodyLength;

	p1 = idatabuf;
	memcpy(p1, headerBuffer, headerLength);
	p1 += headerLength;
	memcpy(p1, bodyBuffer, bodyLength);

	nrfx_err_t err;
	nrfx_spim_xfer_desc_t xfer_desc = {
		.p_tx_buffer = idatabuf,
		.tx_length = idatalength,
		.p_rx_buffer = itempbuf,
		.rx_length = idatalength,
	};

	transfer_finished = false;
	gpio_pin_set_dt(&cs_ctrl.gpio, 1);
	err = nrfx_spim_xfer(&spim, &xfer_desc, 0);
	if (err != NRFX_SUCCESS) {
		LOG_DBG("nrfx_spim_xfer() failed: 0x%08x", err);
		return -EIO;
	}
	while (!transfer_finished);
	gpio_pin_set_dt(&cs_ctrl.gpio, 0);

	return 0;
}

int32_t dw3000_spi_read(uint16_t headerLength, uint8_t* headerBuffer,
						uint16_t readLength, uint8_t* readBuffer)
{
	uint8_t *p1;
	uint32_t idatalength = headerLength + readLength;

	p1 = idatabuf;
	memcpy(p1, headerBuffer, headerLength);
	p1 += headerLength;
	memset(p1, 0x00, readLength);

	nrfx_err_t err;
	nrfx_spim_xfer_desc_t xfer_desc = {
		.p_tx_buffer = idatabuf,
		.tx_length = idatalength,
		.p_rx_buffer = itempbuf,
		.rx_length = idatalength,
	};

	transfer_finished = false;
	gpio_pin_set_dt(&cs_ctrl.gpio, 1);
	err = nrfx_spim_xfer(&spim, &xfer_desc, 0);
	if (err != NRFX_SUCCESS) {
		LOG_DBG("nrfx_spim_xfer() failed: 0x%08x", err);
		return -EIO;
	}
	while (!transfer_finished);
	gpio_pin_set_dt(&cs_ctrl.gpio, 0);

	memcpy(readBuffer, itempbuf + headerLength, readLength);

#if (CONFIG_SOC_NRF52840_QIAA)
	/*
	 *  This is a hack to handle the corrupted response frame through the
	 * nRF52840's SPI3. See this project's issue-log
	 * (https://github.com/foldedtoad/dwm3000/issues/2) for details. The delay
	 * value is set in the CMakeList.txt file for this subproject.
	 */
	if (ret == 0) {
		for (volatile int i = 0; i < TX_WAIT_RESP_NRF52840_DELAY; i++) {
			/* spin */
		}
	}
#endif

	return 0;
}

void dw3000_spi_wakeup()
{
#if KERNEL_VERSION_MAJOR > 3                                                   \
	|| (KERNEL_VERSION_MAJOR == 3 && KERNEL_VERSION_MINOR >= 4)
	gpio_pin_set_dt(&cs_ctrl.gpio, 0);
	k_sleep(K_USEC(500));
	gpio_pin_set_dt(&cs_ctrl.gpio, 1);
#else
	gpio_pin_set_dt(&cs_ctrl->gpio, 0);
	k_sleep(K_USEC(500));
	gpio_pin_set_dt(&cs_ctrl->gpio, 1);
#endif
}
