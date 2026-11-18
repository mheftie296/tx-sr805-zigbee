/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *
 * @brief ISCP (Integra Serial Control Protocol) link to an Onkyo TX-SR805.
 *
 * Commands are plain ASCII: '!' <unit> <command> <parameter> '\r', for example
 * "!1PWR01\r". The TX-SR805 terminates replies with 0x1A. The link runs at
 * 9600 baud, 8 data bits, no parity, 1 stop bit.
 */

#ifndef ONKYO_ISCP_H
#define ONKYO_ISCP_H 1

#include <stdbool.h>
#include <stdint.h>

/** Identifier of the receiver all commands are addressed to. */
#define ONKYO_ISCP_UNIT_ID "1"

/** Master volume range as reported by the receiver with the MVL command. */
#define ONKYO_ISCP_VOLUME_MIN 0x00
#define ONKYO_ISCP_VOLUME_MAX 0x64

/** Input selector values of the TX-SR805 (SLI command). */
#define ONKYO_ISCP_INPUT_CBL_SAT 0x01
#define ONKYO_ISCP_INPUT_GAME_TV 0x02
#define ONKYO_ISCP_INPUT_PC      0x05
#define ONKYO_ISCP_INPUT_BD_DVD  0x10

/** Receiver state as reported back over the link. */
struct onkyo_iscp_state {
	/** Set when the receiver has reported its power state. */
	bool power_valid;
	bool power;
	/** Set when the receiver has reported its mute state. */
	bool mute_valid;
	bool mute;
	/** Set when the receiver has reported the active input. */
	bool input_valid;
	uint8_t input;
	/** Set when the receiver has reported the master volume. */
	bool volume_valid;
	uint8_t volume;
};

/**
 * @brief Callback invoked from thread context when the receiver reports state.
 *
 * @param[in] state  Latest known receiver state.
 */
typedef void (*onkyo_iscp_state_cb_t)(const struct onkyo_iscp_state *state);

/**
 * @brief Initialize the ISCP link.
 *
 * Enables the receiver UART and starts the worker that serializes outgoing
 * commands and parses the replies.
 *
 * @param[in] state_cb  Callback for state reported by the receiver. May be NULL.
 *
 * @retval 0        Link is ready.
 * @retval -ENODEV  The receiver UART is not available.
 */
int onkyo_iscp_init(onkyo_iscp_state_cb_t state_cb);

/**
 * @brief Queue "!1PWR<00|01>", turning the receiver on or into standby.
 *
 * @retval 0         Command queued.
 * @retval -EAGAIN   The transmit queue is full.
 * @retval -ENODEV   The link is not initialized.
 */
int onkyo_iscp_power_set(bool on);

/** @brief Queue "!1PWRQSTN", asking the receiver for its power state. */
int onkyo_iscp_power_query(void);

/**
 * @brief Queue "!1MVL<xx>", setting the master volume.
 *
 * @param[in] volume  Volume in the receiver's 0-100 range.
 */
int onkyo_iscp_volume_set(uint8_t volume);

/**
 * @brief Queue "!1MVL<UP|DOWN>", stepping the master volume by one step.
 *
 * @param[in] up  True to raise, false to lower the volume.
 */
int onkyo_iscp_volume_step(bool up);

/** @brief Queue "!1MVLQSTN", asking the receiver for its master volume. */
int onkyo_iscp_volume_query(void);

/** @brief Queue "!1AMT<00|01>", muting or unmuting the output. */
int onkyo_iscp_mute_set(bool mute);

/** @brief Queue "!1AMTQSTN", asking the receiver for its mute state. */
int onkyo_iscp_mute_query(void);

/**
 * @brief Queue "!1SLI<xx>", selecting an input.
 *
 * @param[in] input  Input selector value, see @ref ONKYO_ISCP_INPUT_CBL_SAT.
 */
int onkyo_iscp_input_select(uint8_t input);

/** @brief Queue "!1SLIQSTN", asking the receiver for the active input. */
int onkyo_iscp_input_query(void);

/** @brief Queue "!1OSDMENU", opening the on-screen display menu. */
int onkyo_iscp_osd_menu(void);

#endif /* ONKYO_ISCP_H */
