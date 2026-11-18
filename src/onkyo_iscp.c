/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include "onkyo_iscp.h"

LOG_MODULE_DECLARE(app, LOG_LEVEL_INF);

/* '!' <unit> <3 character command> <parameter>, the longest parameter is "QSTN". */
#define ISCP_FRAME_MAX_LEN 16

/* Offset of the command within a frame, past '!' and the unit identifier. */
#define ISCP_COMMAND_OFFSET 2
#define ISCP_COMMAND_LEN    3

#define ISCP_START          '!'
#define ISCP_TX_TERMINATOR  '\r'
#define ISCP_RX_TERMINATOR  0x1A

/* Outgoing commands that can wait for the UART to become free. */
#define ISCP_TX_QUEUE_LEN 8

/* Size of the buffer received bytes are collected in before they are parsed. */
#define ISCP_RX_RING_SIZE 64

/* The worker runs below the Zigbee and main threads, it only ever handles
 * serial traffic.
 */
#define ISCP_THREAD_PRIORITY   K_PRIO_PREEMPT(10)
#define ISCP_THREAD_STACK_SIZE 1024

struct iscp_frame {
	char data[ISCP_FRAME_MAX_LEN];
	uint8_t len;
};

static const struct device *const iscp_uart = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart1));

K_MSGQ_DEFINE(iscp_tx_queue, sizeof(struct iscp_frame), ISCP_TX_QUEUE_LEN, 1);
RING_BUF_DECLARE(iscp_rx_ring, ISCP_RX_RING_SIZE);
static K_SEM_DEFINE(iscp_work_sem, 0, 1);

static onkyo_iscp_state_cb_t iscp_state_cb;
static struct onkyo_iscp_state iscp_state;

/* Frame being assembled from received bytes, always NUL terminated. */
static char iscp_rx_frame[ISCP_FRAME_MAX_LEN + 1];
static uint8_t iscp_rx_frame_len;

static bool iscp_ready;

static void iscp_wake(void)
{
	(void)k_sem_give(&iscp_work_sem);
}

static bool iscp_parse_bool2(const char *text, bool *value)
{
	if ((text[0] != '0') || ((text[1] != '0') && (text[1] != '1'))) {
		return false;
	}

	*value = (text[1] == '1');

	return true;
}

static bool iscp_parse_hex2(const char *text, uint8_t *value)
{
	uint8_t result = 0U;

	for (uint8_t i = 0U; i < 2U; i++) {
		char digit = text[i];

		result <<= 4;

		if ((digit >= '0') && (digit <= '9')) {
			result |= (uint8_t)(digit - '0');
		} else if ((digit >= 'A') && (digit <= 'F')) {
			result |= (uint8_t)(digit - 'A' + 10);
		} else if ((digit >= 'a') && (digit <= 'f')) {
			result |= (uint8_t)(digit - 'a' + 10);
		} else {
			return false;
		}
	}

	*value = result;

	return true;
}

static void iscp_handle_frame(const char *frame, uint8_t len)
{
	const char *command;
	const char *parameter;
	uint8_t parameter_len;
	bool bool_value;
	uint8_t value;
	bool changed = false;

	LOG_INF("ISCP RX: %s", frame);

	if (len < (ISCP_COMMAND_OFFSET + ISCP_COMMAND_LEN)) {
		return;
	}

	command = frame + ISCP_COMMAND_OFFSET;
	parameter = command + ISCP_COMMAND_LEN;
	parameter_len = len - (ISCP_COMMAND_OFFSET + ISCP_COMMAND_LEN);

	if ((strncmp(command, "PWR", ISCP_COMMAND_LEN) == 0) && (parameter_len >= 2U) &&
	    iscp_parse_bool2(parameter, &bool_value)) {
		iscp_state.power = bool_value;
		iscp_state.power_valid = true;
		changed = true;
	} else if ((strncmp(command, "AMT", ISCP_COMMAND_LEN) == 0) && (parameter_len >= 2U) &&
		   iscp_parse_bool2(parameter, &bool_value)) {
		iscp_state.mute = bool_value;
		iscp_state.mute_valid = true;
		changed = true;
	} else if ((strncmp(command, "MVL", ISCP_COMMAND_LEN) == 0) && (parameter_len >= 2U) &&
		   iscp_parse_hex2(parameter, &value)) {
		iscp_state.volume = value;
		iscp_state.volume_valid = true;
		changed = true;
	} else if ((strncmp(command, "SLI", ISCP_COMMAND_LEN) == 0) && (parameter_len >= 2U) &&
		   iscp_parse_hex2(parameter, &value)) {
		iscp_state.input = value;
		iscp_state.input_valid = true;
		changed = true;
	}

	if (changed && (iscp_state_cb != NULL)) {
		iscp_state_cb(&iscp_state);
	}
}

static void iscp_rx_byte(char byte)
{
	if (byte == ISCP_START) {
		iscp_rx_frame[0] = byte;
		iscp_rx_frame_len = 1U;
		return;
	}

	if (iscp_rx_frame_len == 0U) {
		/* Not inside a frame, ignore the byte. */
		return;
	}

	/* The TX-SR805 terminates replies with SUB (0x1A). Accept CR too for
	 * compatibility with receivers that use the documented terminator.
	 */
	if ((byte == ISCP_RX_TERMINATOR) || (byte == ISCP_TX_TERMINATOR)) {
		iscp_rx_frame[iscp_rx_frame_len] = '\0';
		iscp_handle_frame(iscp_rx_frame, iscp_rx_frame_len);
		iscp_rx_frame_len = 0U;
		return;
	}

	if (iscp_rx_frame_len < ISCP_FRAME_MAX_LEN) {
		iscp_rx_frame[iscp_rx_frame_len++] = byte;
	} else {
		/* Longer than any expected reply, drop the frame. */
		iscp_rx_frame_len = 0U;
	}
}

static void iscp_uart_isr(const struct device *dev, void *user_data)
{
	uint8_t chunk[16];
	int received;

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			break;
		}

		do {
			received = uart_fifo_read(dev, chunk, sizeof(chunk));
			if (received > 0) {
				(void)ring_buf_put(&iscp_rx_ring, chunk, (uint32_t)received);
			}
		} while (received > 0);
	}

	iscp_wake();
}

static void iscp_pump_rx(void)
{
	uint8_t chunk[16];
	uint32_t len;

	do {
		len = ring_buf_get(&iscp_rx_ring, chunk, sizeof(chunk));

		for (uint32_t i = 0U; i < len; i++) {
			iscp_rx_byte((char)chunk[i]);
		}
	} while (len > 0U);
}

static void iscp_pump_tx(void)
{
	struct iscp_frame frame;

	while (k_msgq_get(&iscp_tx_queue, &frame, K_NO_WAIT) == 0) {
		for (uint8_t i = 0U; i < frame.len; i++) {
			uart_poll_out(iscp_uart, (uint8_t)frame.data[i]);
		}
	}
}

static void iscp_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		(void)k_sem_take(&iscp_work_sem, K_FOREVER);

		iscp_pump_rx();
		iscp_pump_tx();
	}
}

K_THREAD_DEFINE(iscp_thread, ISCP_THREAD_STACK_SIZE, iscp_thread_fn, NULL, NULL, NULL,
		ISCP_THREAD_PRIORITY, 0, 0);

static int iscp_send(const char *command, const char *parameter)
{
	struct iscp_frame frame;
	int len;

	if (!iscp_ready) {
		return -ENODEV;
	}

	len = snprintk(frame.data, sizeof(frame.data), "%c%s%s%s%c", ISCP_START,
		       ONKYO_ISCP_UNIT_ID, command, parameter, ISCP_TX_TERMINATOR);
	if ((len < 0) || (len >= (int)sizeof(frame.data))) {
		LOG_ERR("ISCP command %s%s does not fit in a frame", command, parameter);
		return -EINVAL;
	}

	frame.len = (uint8_t)len;

	if (k_msgq_put(&iscp_tx_queue, &frame, K_NO_WAIT) != 0) {
		LOG_WRN("ISCP transmit queue full, dropping %s%s", command, parameter);
		return -EAGAIN;
	}

	LOG_INF("ISCP TX: %s%s", command, parameter);

	iscp_wake();

	return 0;
}

static int iscp_send_hex2(const char *command, uint8_t value)
{
	char parameter[3];

	(void)snprintk(parameter, sizeof(parameter), "%02X", value);

	return iscp_send(command, parameter);
}

int onkyo_iscp_init(onkyo_iscp_state_cb_t state_cb)
{
	if (iscp_uart == NULL) {
		LOG_ERR("Receiver UART is not enabled in the devicetree");
		return -ENODEV;
	}

	if (!device_is_ready(iscp_uart)) {
		LOG_ERR("Receiver UART is not ready");
		return -ENODEV;
	}

	iscp_state_cb = state_cb;

	uart_irq_callback_set(iscp_uart, iscp_uart_isr);
	uart_irq_rx_enable(iscp_uart);

	iscp_ready = true;

	return 0;
}

int onkyo_iscp_power_set(bool on)
{
	return iscp_send("PWR", on ? "01" : "00");
}

int onkyo_iscp_power_query(void)
{
	return iscp_send("PWR", "QSTN");
}

int onkyo_iscp_volume_set(uint8_t volume)
{
	if (volume > ONKYO_ISCP_VOLUME_MAX) {
		volume = ONKYO_ISCP_VOLUME_MAX;
	}

	return iscp_send_hex2("MVL", volume);
}

int onkyo_iscp_volume_step(bool up)
{
	return iscp_send("MVL", up ? "UP" : "DOWN");
}

int onkyo_iscp_volume_query(void)
{
	return iscp_send("MVL", "QSTN");
}

int onkyo_iscp_mute_set(bool mute)
{
	return iscp_send("AMT", mute ? "01" : "00");
}

int onkyo_iscp_mute_query(void)
{
	return iscp_send("AMT", "QSTN");
}

int onkyo_iscp_input_select(uint8_t input)
{
	return iscp_send_hex2("SLI", input);
}

int onkyo_iscp_input_query(void)
{
	return iscp_send("SLI", "QSTN");
}

int onkyo_iscp_osd_menu(void)
{
	return iscp_send("OSD", "MENU");
}
