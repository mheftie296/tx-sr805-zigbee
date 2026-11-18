/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *
 * @brief Zigbee controller for an Onkyo TX-SR805 AV receiver.
 *
 * The receiver is driven over its RS-232 port with the ISCP protocol. The
 * Zigbee endpoint exposes:
 *
 * - On/Off (0x0006): receiver power,
 * - Level Control (0x0008): master volume,
 * - Onkyo AVR control (0xFC00, manufacturer specific): input selection, mute,
 *   volume stepping and the on-screen display menu.
 *
 * On/Off and Level Control make the receiver look like a dimmable light, so a
 * controller that builds its entities from the clusters, for example Home
 * Assistant, controls the receiver power and its master volume without any
 * device specific handling.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <dk_buttons_and_leds.h>

#include <zboss_api.h>
#include <zboss_api_addons.h>
#include <zigbee/zigbee_error_handler.h>
#include <zigbee/zigbee_app_utils.h>
#include <zb_nrf_platform.h>

#include "onkyo_iscp.h"

/* Device endpoint used to expose the receiver control clusters. */
#define ONKYO_AVR_ENDPOINT 10

/* Manufacturer specific cluster carrying the Onkyo specific commands. */
#define ZB_ZCL_CLUSTER_ID_ONKYO_AVR 0xFC00

/* Manufacturer code of that cluster. Cluster IDs at and above 0xFC00 are
 * manufacturer specific, and the code names the vendor whose specification the
 * cluster follows, both for its commands and for its attributes.
 */
#define ONKYO_AVR_MANUF_CODE CONFIG_ONKYO_AVR_MANUFACTURER_CODE

/* Attributes of the Onkyo AVR control cluster. */
#define ZB_ONKYO_AVR_ATTR_INPUT_ID 0x0000
#define ZB_ONKYO_AVR_ATTR_MUTE_ID  0x0001

/* Commands of the Onkyo AVR control cluster. */
#define ZB_ONKYO_AVR_CMD_INPUT_SELECT 0x00
#define ZB_ONKYO_AVR_CMD_MUTE         0x01
#define ZB_ONKYO_AVR_CMD_VOLUME_STEP  0x02
#define ZB_ONKYO_AVR_CMD_OSD_MENU     0x03

/* Payload of the Mute command that flips the current mute state. */
#define ZB_ONKYO_AVR_MUTE_TOGGLE 0x02

/* Number of server and client clusters on the receiver endpoint. */
#define ONKYO_AVR_IN_CLUSTER_NUM  5
#define ONKYO_AVR_OUT_CLUSTER_NUM 0

/* Standard state attributes that Home Assistant can configure for reporting. */
#define ONKYO_AVR_REPORT_ATTR_COUNT \
	(ZB_ZCL_ON_OFF_REPORT_ATTR_COUNT + ZB_ZCL_LEVEL_CONTROL_REPORT_ATTR_COUNT)
#define ONKYO_AVR_CVC_ATTR_COUNT 1

/* Device version of the level controllable output device, see the Zigbee Home
 * Automation profile specification.
 */
#define ONKYO_AVR_DEVICE_VERSION 0

/* Type of power sources available for the device.
 * For possible values see section 3.2.2.2.8 of ZCL specification.
 */
#define ONKYO_AVR_INIT_BASIC_POWER_SOURCE ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE

/* Device identification reported in the Basic cluster. Controllers use these
 * attributes to name the device and to match device specific handling, and
 * they are what makes the device known instead of an anonymous node.
 */
#define ONKYO_AVR_MANUFACTURER_NAME "Nordic Semiconductor ASA"
#define ONKYO_AVR_MODEL_IDENTIFIER  "Onkyo AVR controller"
#define ONKYO_AVR_DATE_CODE         "20260101"
#define ONKYO_AVR_SW_BUILD_ID       "1.0.0"

#define ONKYO_AVR_APP_VERSION 1
#define ONKYO_AVR_HW_VERSION  1

/* Map the full Home Assistant brightness range to this useful receiver
 * volume range. This gives finer control while avoiding unused extremes.
 */
#define ONKYO_AVR_VOLUME_CONTROL_MIN 30
#define ONKYO_AVR_VOLUME_CONTROL_MAX 70

/* Maximum length of the DateCode, SWBuildId and LocationDescription strings. */
#define ONKYO_AVR_BASIC_STRING_MAX_LEN 16

/* A ZCL character string attribute keeps its length in the first byte, so its
 * storage is one byte longer than the longest string it can hold.
 */
#define ONKYO_AVR_ZCL_STRING_SIZE(max_len) ((max_len) + 1)

/* Delay before the On/Off part of a level control command is applied to the
 * receiver. ZBOSS assigns it to the On/Off attribute only once the last level
 * step has been reported, so the attribute is read after it has settled.
 */
#define ONKYO_AVR_LEVEL_POWER_DELAY_MS 100

/* The Pro Micro nRF52840 has a single LED and no user buttons, so that LED
 * carries both the network status and the identify indication, and Identify
 * mode and factory reset are only bound to buttons on boards that have them.
 */
#define ONKYO_AVR_HAS_BUTTONS DT_NODE_EXISTS(DT_PATH(buttons))

#define ZIGBEE_NETWORK_STATE_LED DK_LED1
#define IDENTIFY_LED             DK_LED1

#if ONKYO_AVR_HAS_BUTTONS
#define IDENTIFY_MODE_BUTTON DK_BTN4_MSK
#define FACTORY_RESET_BUTTON IDENTIFY_MODE_BUTTON
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* Attributes of the Onkyo AVR control cluster. */
struct onkyo_avr_attrs {
	zb_uint8_t input;
	zb_uint8_t mute;
};

/* Main application customizable context.
 * Stores all settings and static values.
 */
struct onkyo_device_ctx {
	zb_zcl_basic_attrs_t basic_attr;
	zb_zcl_identify_attrs_t identify_attr;
	zb_zcl_on_off_attrs_t on_off_attr;
	zb_zcl_level_control_attrs_t level_control_attr;
	struct onkyo_avr_attrs onkyo_avr_attr;

	/* Basic cluster attributes that zb_zcl_basic_attrs_t does not carry. */
	zb_uint8_t basic_app_version;
	zb_uint8_t basic_stack_version;
	zb_uint8_t basic_hw_version;
	zb_uint8_t basic_mf_name[ONKYO_AVR_ZCL_STRING_SIZE(
		ZB_ZCL_CLUSTER_ID_BASIC_MANUFACTURER_NAME_MAX_LEN)];
	zb_uint8_t basic_model_id[ONKYO_AVR_ZCL_STRING_SIZE(
		ZB_ZCL_CLUSTER_ID_BASIC_MODEL_IDENTIFIER_MAX_LEN)];
	zb_uint8_t basic_date_code[ONKYO_AVR_ZCL_STRING_SIZE(
		ONKYO_AVR_BASIC_STRING_MAX_LEN)];
	zb_uint8_t basic_sw_build_id[ONKYO_AVR_ZCL_STRING_SIZE(
		ONKYO_AVR_BASIC_STRING_MAX_LEN)];
	zb_uint8_t basic_location_id[ONKYO_AVR_ZCL_STRING_SIZE(
		ONKYO_AVR_BASIC_STRING_MAX_LEN)];
	zb_uint8_t basic_ph_env;
};

/* Zigbee device application context storage. */
static struct onkyo_device_ctx dev_ctx;
ZBOSS_DEVICE_DECLARE_REPORTING_CTX(onkyo_avr_reporting_ctx,
				   ONKYO_AVR_REPORT_ATTR_COUNT);
ZBOSS_DEVICE_DECLARE_LEVEL_CONTROL_CTX(onkyo_avr_cvc_ctx,
				       ONKYO_AVR_CVC_ATTR_COUNT);

/* Power state last applied to the receiver, compared against the On/Off
 * attribute so that a state the receiver already has is not commanded again.
 */
static bool power_applied_valid;
static bool power_applied;

/* Last state reported by the receiver. Written by the ISCP worker and applied
 * from the Zigbee thread, so both sides use the lock below.
 */
static struct onkyo_iscp_state iscp_state;
static K_MUTEX_DEFINE(iscp_state_lock);

ZB_ZCL_DECLARE_IDENTIFY_ATTRIB_LIST(
	identify_attr_list,
	&dev_ctx.identify_attr.identify_time);

ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT(
	basic_attr_list,
	&dev_ctx.basic_attr.zcl_version,
	&dev_ctx.basic_app_version,
	&dev_ctx.basic_stack_version,
	&dev_ctx.basic_hw_version,
	dev_ctx.basic_mf_name,
	dev_ctx.basic_model_id,
	dev_ctx.basic_date_code,
	&dev_ctx.basic_attr.power_source,
	dev_ctx.basic_location_id,
	&dev_ctx.basic_ph_env,
	dev_ctx.basic_sw_build_id);

ZB_ZCL_DECLARE_ON_OFF_ATTRIB_LIST(
	on_off_attr_list,
	&dev_ctx.on_off_attr.on_off);

ZB_ZCL_DECLARE_LEVEL_CONTROL_ATTRIB_LIST(
	level_control_attr_list,
	&dev_ctx.level_control_attr.current_level,
	&dev_ctx.level_control_attr.remaining_time);

ZB_ZCL_START_DECLARE_ATTRIB_LIST(onkyo_avr_attr_list)
	ZB_ZCL_SET_MANUF_SPEC_ATTR_DESC(ZB_ONKYO_AVR_ATTR_INPUT_ID,
					ZB_ZCL_ATTR_TYPE_8BIT_ENUM,
					ZB_ZCL_ATTR_ACCESS_READ_ONLY,
					ONKYO_AVR_MANUF_CODE,
					&dev_ctx.onkyo_avr_attr.input)
	ZB_ZCL_SET_MANUF_SPEC_ATTR_DESC(ZB_ONKYO_AVR_ATTR_MUTE_ID,
					ZB_ZCL_ATTR_TYPE_BOOL,
					ZB_ZCL_ATTR_ACCESS_READ_ONLY,
					ONKYO_AVR_MANUF_CODE,
					&dev_ctx.onkyo_avr_attr.mute)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;

static void onkyo_avr_cluster_init_server(void);
static zb_bool_t onkyo_avr_cluster_handler(zb_uint8_t param);

/* Resolved by ZB_ZCL_CLUSTER_DESC() through the cluster ID token. */
#define ZB_ZCL_CLUSTER_ID_ONKYO_AVR_SERVER_ROLE_INIT onkyo_avr_cluster_init_server
#define ZB_ZCL_CLUSTER_ID_ONKYO_AVR_CLIENT_ROLE_INIT (zb_zcl_cluster_init_t)NULL

zb_zcl_cluster_desc_t onkyo_avr_clusters[] = {
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_IDENTIFY,
		ZB_ZCL_ARRAY_SIZE(identify_attr_list, zb_zcl_attr_t),
		identify_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID
	),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_ARRAY_SIZE(basic_attr_list, zb_zcl_attr_t),
		basic_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID
	),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_ON_OFF,
		ZB_ZCL_ARRAY_SIZE(on_off_attr_list, zb_zcl_attr_t),
		on_off_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID
	),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
		ZB_ZCL_ARRAY_SIZE(level_control_attr_list, zb_zcl_attr_t),
		level_control_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ZB_ZCL_MANUF_CODE_INVALID
	),
	ZB_ZCL_CLUSTER_DESC(
		ZB_ZCL_CLUSTER_ID_ONKYO_AVR,
		ZB_ZCL_ARRAY_SIZE(onkyo_avr_attr_list, zb_zcl_attr_t),
		onkyo_avr_attr_list,
		ZB_ZCL_CLUSTER_SERVER_ROLE,
		ONKYO_AVR_MANUF_CODE
	),
};

/* The cluster counts are pasted into a type name by ZB_DECLARE_SIMPLE_DESC(),
 * so they are spelled out here. They must match the cluster list above.
 */
ZB_DECLARE_SIMPLE_DESC(5, 0);
ZB_AF_SIMPLE_DESC_TYPE(5, 0)
simple_desc_onkyo_avr_ep = {
	ONKYO_AVR_ENDPOINT,
	ZB_AF_HA_PROFILE_ID,
	/* Dimmable light: the device type that a controller turns into a single
	 * dimmable output, which is the receiver power and master volume. A level
	 * controllable output would only be usable as a plain switch, because
	 * controllers expose a light only for the lighting device types.
	 */
	ZB_HA_DIMMABLE_LIGHT_DEVICE_ID,
	ONKYO_AVR_DEVICE_VERSION,
	0,
	ONKYO_AVR_IN_CLUSTER_NUM,
	ONKYO_AVR_OUT_CLUSTER_NUM,
	{
		ZB_ZCL_CLUSTER_ID_BASIC,
		ZB_ZCL_CLUSTER_ID_IDENTIFY,
		ZB_ZCL_CLUSTER_ID_ON_OFF,
		ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
		ZB_ZCL_CLUSTER_ID_ONKYO_AVR,
	}
};

ZB_AF_DECLARE_ENDPOINT_DESC(
	onkyo_avr_ep,
	ONKYO_AVR_ENDPOINT,
	ZB_AF_HA_PROFILE_ID,
	0,
	NULL,
	ZB_ZCL_ARRAY_SIZE(onkyo_avr_clusters, zb_zcl_cluster_desc_t),
	onkyo_avr_clusters,
	(zb_af_simple_desc_1_1_t *)&simple_desc_onkyo_avr_ep,
	ONKYO_AVR_REPORT_ATTR_COUNT,
	onkyo_avr_reporting_ctx,
	ONKYO_AVR_CVC_ATTR_COUNT,
	onkyo_avr_cvc_ctx);

ZBOSS_DECLARE_DEVICE_CTX_1_EP(onkyo_avr_ctx, onkyo_avr_ep);

/**@brief Function for initializing all clusters attributes. */
static void onkyo_clusters_attr_init(void)
{
	/* Some controllers write CurrentLevel directly instead of using the
	 * Level Control commands. The standard ZBOSS list declares it read-only,
	 * but this application uses writes to drive the receiver volume.
	 */
	level_control_attr_list[1].access =
		(ZB_ZCL_ATTR_ACCESS_READ_WRITE | ZB_ZCL_ATTR_ACCESS_REPORTING |
		 ZB_ZCL_ATTR_ACCESS_SCENE);

	/* Basic cluster attributes data. */
	dev_ctx.basic_attr.zcl_version = ZB_ZCL_VERSION;
	dev_ctx.basic_attr.power_source = ONKYO_AVR_INIT_BASIC_POWER_SOURCE;

	dev_ctx.basic_app_version = ONKYO_AVR_APP_VERSION;
	dev_ctx.basic_stack_version = ZB_ZCL_BASIC_STACK_VERSION_DEFAULT_VALUE;
	dev_ctx.basic_hw_version = ONKYO_AVR_HW_VERSION;
	dev_ctx.basic_ph_env = ZB_ZCL_BASIC_PHYSICAL_ENVIRONMENT_DEFAULT_VALUE;

	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_mf_name, ONKYO_AVR_MANUFACTURER_NAME,
			      ZB_ZCL_STRING_CONST_SIZE(ONKYO_AVR_MANUFACTURER_NAME));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_model_id, ONKYO_AVR_MODEL_IDENTIFIER,
			      ZB_ZCL_STRING_CONST_SIZE(ONKYO_AVR_MODEL_IDENTIFIER));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_date_code, ONKYO_AVR_DATE_CODE,
			      ZB_ZCL_STRING_CONST_SIZE(ONKYO_AVR_DATE_CODE));
	ZB_ZCL_SET_STRING_VAL(dev_ctx.basic_sw_build_id, ONKYO_AVR_SW_BUILD_ID,
			      ZB_ZCL_STRING_CONST_SIZE(ONKYO_AVR_SW_BUILD_ID));
	ZB_ZCL_SET_STRING_LENGTH(dev_ctx.basic_location_id, 0);

	/* Identify cluster attributes data. */
	dev_ctx.identify_attr.identify_time =
		ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE;

	/* On/Off cluster attributes data. */
	dev_ctx.on_off_attr.on_off = ZB_FALSE;

	/* Level Control cluster attributes data. */
	dev_ctx.level_control_attr.current_level = ZB_ZCL_LEVEL_CONTROL_LEVEL_MIN_VALUE;
	dev_ctx.level_control_attr.remaining_time = 0;

	/* Onkyo AVR control cluster attributes data. The real values are read
	 * back from the receiver once the serial link is up.
	 */
	dev_ctx.onkyo_avr_attr.input = ONKYO_ISCP_INPUT_BD_DVD;
	dev_ctx.onkyo_avr_attr.mute = ZB_FALSE;
}

/**@brief Convert a ZCL level to the configured receiver volume range.
 *
 * @param[in] level  Level in the ZCL 0-254 range.
 *
 * @return Master volume in the receiver's 0-100 range.
 */
static uint8_t onkyo_volume_from_level(uint8_t level)
{
	return (uint8_t)(ONKYO_AVR_VOLUME_CONTROL_MIN +
			 ((uint16_t)level *
			  (ONKYO_AVR_VOLUME_CONTROL_MAX - ONKYO_AVR_VOLUME_CONTROL_MIN) +
			  127U) /
			 254U);
}

/**@brief Convert the receiver's master volume to a ZCL level.
 *
 * @param[in] volume  Master volume in the receiver's 0-100 range.
 *
 * @return Level in the ZCL 0-254 range.
 */
static uint8_t level_from_onkyo_volume(uint8_t volume)
{
	uint8_t clamped_volume = MAX(ONKYO_AVR_VOLUME_CONTROL_MIN,
				     MIN(volume, ONKYO_AVR_VOLUME_CONTROL_MAX));

	return (uint8_t)(((uint16_t)(clamped_volume - ONKYO_AVR_VOLUME_CONTROL_MIN) *
			  254U +
			  (ONKYO_AVR_VOLUME_CONTROL_MAX - ONKYO_AVR_VOLUME_CONTROL_MIN) / 2U) /
			 (ONKYO_AVR_VOLUME_CONTROL_MAX - ONKYO_AVR_VOLUME_CONTROL_MIN));
}

/**@brief Turn the receiver on or into standby.
 *
 * @param[in] on  Boolean receiver power state.
 */
static void onkyo_avr_power_set(zb_bool_t on)
{
	power_applied = (on != ZB_FALSE);
	power_applied_valid = true;

	ZB_ZCL_SET_ATTRIBUTE(ONKYO_AVR_ENDPOINT, ZB_ZCL_CLUSTER_ID_ON_OFF,
			     ZB_ZCL_CLUSTER_SERVER_ROLE, ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
			     (zb_uint8_t *)&on, ZB_FALSE);

	(void)onkyo_iscp_power_set(on);
}

/**@brief Apply the On/Off attribute to the receiver.
 *
 * The *WithOnOff level control commands also switch the output on and off, but
 * ZBOSS assigns that part of the command to the On/Off attribute itself and
 * does not report it to the application the way it reports the On/Off commands.
 * The receiver is therefore commanded from the attribute whenever a level
 * command has been processed, and only when the attribute holds a state the
 * receiver does not have yet.
 *
 * @param[in]  param  Unused parameter, required by ZBOSS scheduler API.
 */
static void onkyo_avr_power_apply(zb_uint8_t param)
{
	zb_bool_t on = dev_ctx.on_off_attr.on_off;

	ZVUNUSED(param);

	if (power_applied_valid && (power_applied == (on != ZB_FALSE))) {
		return;
	}

	LOG_INF("Receiver power: %s", on ? "on" : "standby");
	onkyo_avr_power_set(on);
}

/**@brief Set the receiver master volume.
 *
 * @param[in] level  Level in the ZCL 0-254 range.
 */
static void onkyo_avr_level_set(zb_uint16_t level)
{
	zb_uint8_t level8 = (zb_uint8_t)MIN(level, ZB_ZCL_LEVEL_CONTROL_LEVEL_MAX_VALUE);

	ZB_ZCL_SET_ATTRIBUTE(ONKYO_AVR_ENDPOINT, ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
			     ZB_ZCL_CLUSTER_SERVER_ROLE,
			     ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
			     &level8, ZB_FALSE);

	(void)onkyo_iscp_volume_set(onkyo_volume_from_level(level8));
}

/**@brief Step the receiver master volume by one step.
 *
 * @param[in] up  True to raise, false to lower the volume.
 */
static void onkyo_avr_volume_step(bool up)
{
	zb_uint8_t level = dev_ctx.level_control_attr.current_level;

	if (up) {
		level = (zb_uint8_t)MIN(level + 1U, ZB_ZCL_LEVEL_CONTROL_LEVEL_MAX_VALUE);
	} else if (level > ZB_ZCL_LEVEL_CONTROL_LEVEL_MIN_VALUE) {
		level--;
	}

	ZB_ZCL_SET_ATTRIBUTE(ONKYO_AVR_ENDPOINT, ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
			     ZB_ZCL_CLUSTER_SERVER_ROLE,
			     ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
			     &level, ZB_FALSE);

	(void)onkyo_iscp_volume_step(up);
}

/**@brief Set an attribute of the Onkyo AVR control cluster.
 *
 * The cluster and its attributes are manufacturer specific, so the attributes
 * are addressed with the manufacturer code of the cluster.
 *
 * @param[in] attr_id  Attribute ID, see @ref ZB_ONKYO_AVR_ATTR_INPUT_ID.
 * @param[in] value    Pointer to the new attribute value.
 */
static void onkyo_avr_attr_set(zb_uint16_t attr_id, zb_uint8_t *value)
{
	(void)zb_zcl_set_attr_val_manuf(ONKYO_AVR_ENDPOINT,
					ZB_ZCL_CLUSTER_ID_ONKYO_AVR,
					ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id,
					ONKYO_AVR_MANUF_CODE, value, ZB_FALSE);
}

/**@brief Mute or unmute the receiver.
 *
 * @param[in] value  0 to unmute, 1 to mute, @ref ZB_ONKYO_AVR_MUTE_TOGGLE to flip
 *                   the current state.
 */
static void onkyo_avr_mute_set(zb_uint8_t value)
{
	zb_uint8_t mute;

	if (value == ZB_ONKYO_AVR_MUTE_TOGGLE) {
		mute = dev_ctx.onkyo_avr_attr.mute ? ZB_FALSE : ZB_TRUE;
	} else {
		mute = (value != 0U) ? ZB_TRUE : ZB_FALSE;
	}

	onkyo_avr_attr_set(ZB_ONKYO_AVR_ATTR_MUTE_ID, &mute);

	(void)onkyo_iscp_mute_set(mute != ZB_FALSE);
}

/**@brief Select the receiver input.
 *
 * @param[in] input  Input selector value, see @ref ONKYO_ISCP_INPUT_CBL_SAT.
 */
static void onkyo_avr_input_set(zb_uint8_t input)
{
	onkyo_avr_attr_set(ZB_ONKYO_AVR_ATTR_INPUT_ID, &input);

	(void)onkyo_iscp_input_select(input);
}

/**@brief Copy the last receiver state into the Zigbee attributes.
 *
 * @param  param  Unused parameter, required by ZBOSS scheduler API.
 */
static void onkyo_apply_receiver_state(zb_uint8_t param)
{
	struct onkyo_iscp_state state;
	zb_uint8_t value;

	ZVUNUSED(param);

	k_mutex_lock(&iscp_state_lock, K_FOREVER);
	state = iscp_state;
	k_mutex_unlock(&iscp_state_lock);

	if (state.power_valid) {
		power_applied = (state.power != 0);
		power_applied_valid = true;

		value = state.power ? ZB_TRUE : ZB_FALSE;
		ZB_ZCL_SET_ATTRIBUTE(ONKYO_AVR_ENDPOINT, ZB_ZCL_CLUSTER_ID_ON_OFF,
				     ZB_ZCL_CLUSTER_SERVER_ROLE,
				     ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &value, ZB_FALSE);
	}

	if (state.volume_valid) {
		value = level_from_onkyo_volume(state.volume);
		ZB_ZCL_SET_ATTRIBUTE(ONKYO_AVR_ENDPOINT, ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
				     ZB_ZCL_CLUSTER_SERVER_ROLE,
				     ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
				     &value, ZB_FALSE);
	}

	if (state.mute_valid) {
		value = state.mute ? ZB_TRUE : ZB_FALSE;
		onkyo_avr_attr_set(ZB_ONKYO_AVR_ATTR_MUTE_ID, &value);
	}

	if (state.input_valid) {
		value = state.input;
		onkyo_avr_attr_set(ZB_ONKYO_AVR_ATTR_INPUT_ID, &value);
	}
}

/**@brief Function for handling ZCL commands.
 *
 * @param[in]   bufid   Reference to the Zigbee stack buffer
 *                      used to pass signal.
 */
static void zcl_device_cb(zb_bufid_t bufid)
{
	zb_uint8_t cluster_id;
	zb_uint8_t attr_id;
	zb_zcl_device_callback_param_t *device_cb_param =
		ZB_BUF_GET_PARAM(bufid, zb_zcl_device_callback_param_t);

	/* Set default response value. */
	device_cb_param->status = RET_OK;

	switch (device_cb_param->device_cb_id) {
	case ZB_ZCL_SET_ATTR_VALUE_CB_ID:
		cluster_id = device_cb_param->cb_param.set_attr_value_param.cluster_id;
		attr_id = device_cb_param->cb_param.set_attr_value_param.attr_id;

		if ((cluster_id == ZB_ZCL_CLUSTER_ID_ON_OFF) &&
		    (attr_id == ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID)) {
			zb_bool_t on = (zb_bool_t)
				device_cb_param->cb_param.set_attr_value_param.values.data8;

			LOG_INF("Receiver power: %s", on ? "on" : "standby");
			onkyo_avr_power_set(on);
		} else if ((cluster_id == ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL) &&
			   (attr_id == ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID)) {
			zb_uint8_t level =
				device_cb_param->cb_param.set_attr_value_param.values.data8;

			LOG_INF("Master volume attribute: %u", (unsigned int)level);
			onkyo_avr_level_set(level);
		} else {
			device_cb_param->status = RET_NOT_IMPLEMENTED;
		}
		break;

	case ZB_ZCL_LEVEL_CONTROL_SET_VALUE_CB_ID:
		LOG_INF("Master volume level: %u",
			(unsigned int)device_cb_param->cb_param.level_control_set_value_param.new_value);
		onkyo_avr_level_set(
			device_cb_param->cb_param.level_control_set_value_param.new_value);

		/* A level command that also switches the output ("*WithOnOff")
		 * has its On/Off part applied to the receiver from the On/Off
		 * attribute, because ZBOSS does not report that part of the
		 * command to the application.
		 */
		ZB_SCHEDULE_APP_ALARM(onkyo_avr_power_apply, 0,
				      ZB_MILLISECONDS_TO_BEACON_INTERVAL(
					      ONKYO_AVR_LEVEL_POWER_DELAY_MS));
		break;

	default:
		device_cb_param->status = RET_NOT_IMPLEMENTED;
		break;
	}
}

/**@brief Handle commands addressed to the Onkyo AVR control cluster.
 *
 * @param[in]   param   Reference to the Zigbee stack buffer with the command.
 *
 * @return ZB_TRUE if the command was handled, ZB_FALSE otherwise.
 */
static zb_bool_t onkyo_avr_cluster_handler(zb_uint8_t param)
{
	zb_zcl_parsed_hdr_t cmd_info;
	zb_uint8_t *payload;
	zb_uint16_t payload_len;
	zb_uint8_t status = ZB_ZCL_STATUS_SUCCESS;

	if (param == ZB_ZCL_GENERAL_GET_CMD_LISTS_PARAM) {
		/* The list of supported commands is not exposed for discovery. */
		return ZB_FALSE;
	}

	ZB_ZCL_COPY_PARSED_HEADER(param, &cmd_info);

	payload = (zb_uint8_t *)zb_buf_begin(param);
	payload_len = zb_buf_len(param);

	switch (cmd_info.cmd_id) {
	case ZB_ONKYO_AVR_CMD_INPUT_SELECT:
		if (payload_len < 1U) {
			status = ZB_ZCL_STATUS_MALFORMED_CMD;
			break;
		}

		LOG_INF("Select input: 0x%02x", (unsigned int)payload[0]);
		onkyo_avr_input_set(payload[0]);
		break;

	case ZB_ONKYO_AVR_CMD_MUTE:
		if (payload_len < 1U) {
			status = ZB_ZCL_STATUS_MALFORMED_CMD;
			break;
		}

		LOG_INF("Mute: 0x%02x", (unsigned int)payload[0]);
		onkyo_avr_mute_set(payload[0]);
		break;

	case ZB_ONKYO_AVR_CMD_VOLUME_STEP:
		if (payload_len < 1U) {
			status = ZB_ZCL_STATUS_MALFORMED_CMD;
			break;
		}

		LOG_INF("Volume step: %s", ((zb_int8_t)payload[0] > 0) ? "up" : "down");
		onkyo_avr_volume_step((zb_int8_t)payload[0] > 0);
		break;

	case ZB_ONKYO_AVR_CMD_OSD_MENU:
		LOG_INF("Open the on-screen display menu");
		(void)onkyo_iscp_osd_menu();
		break;

	default:
		/* Let the stack answer with "unsupported cluster command". */
		return ZB_FALSE;
	}

	ZB_ZCL_PROCESS_COMMAND_FINISH(param, &cmd_info, status);

	return ZB_TRUE;
}

/**@brief Register the handlers of the Onkyo AVR control cluster. */
static void onkyo_avr_cluster_init_server(void)
{
	zb_ret_t err = zb_zcl_add_cluster_handlers(ZB_ZCL_CLUSTER_ID_ONKYO_AVR,
						   ZB_ZCL_CLUSTER_SERVER_ROLE,
						   NULL,
						   NULL,
						   onkyo_avr_cluster_handler);

	if (err != RET_OK) {
		LOG_ERR("Cannot register the Onkyo AVR cluster handler (err: %d)", err);
	}
}

/**@brief Function to toggle the identify LED.
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
static void toggle_identify_led(zb_bufid_t bufid)
{
	static int blink_status;

	(void)dk_set_led(IDENTIFY_LED, (++blink_status) % 2);
	ZB_SCHEDULE_APP_ALARM(toggle_identify_led, bufid,
			      ZB_MILLISECONDS_TO_BEACON_INTERVAL(100));
}

/**@brief Function to handle identify notification events on the endpoint.
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
static void identify_cb(zb_bufid_t bufid)
{
	zb_ret_t zb_err_code;

	if (bufid) {
		/* Schedule a self-scheduling function that will toggle the LED. */
		ZB_SCHEDULE_APP_CALLBACK(toggle_identify_led, bufid);
	} else {
		/* Cancel the toggling function alarm and turn off the LED. */
		zb_err_code = ZB_SCHEDULE_APP_ALARM_CANCEL(toggle_identify_led,
							   ZB_ALARM_ANY_PARAM);
		ZVUNUSED(zb_err_code);

		(void)dk_set_led(IDENTIFY_LED, 0);
	}
}

#if ONKYO_AVR_HAS_BUTTONS
/**@brief Starts identifying the device.
 *
 * @param  bufid  Unused parameter, required by ZBOSS scheduler API.
 */
static void start_identifying(zb_bufid_t bufid)
{
	ZVUNUSED(bufid);

	if (ZB_JOINED()) {
		/* Check if endpoint is in identifying mode,
		 * if not put desired endpoint in identifying mode.
		 */
		if (dev_ctx.identify_attr.identify_time ==
		    ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE) {
			zb_ret_t zb_err_code =
				zb_bdb_finding_binding_target(ONKYO_AVR_ENDPOINT);

			if (zb_err_code == RET_OK) {
				LOG_INF("Enter identify mode");
			} else if (zb_err_code == RET_INVALID_STATE) {
				LOG_WRN("RET_INVALID_STATE - Cannot enter identify mode");
			} else {
				ZB_ERROR_CHECK(zb_err_code);
			}
		} else {
			LOG_INF("Cancel identify mode");
			zb_bdb_finding_binding_target_cancel();
		}
	} else {
		LOG_WRN("Device not in a network - cannot enter identify mode");
	}
}

/**@brief Callback for button events.
 *
 * @param[in]   button_state  Bitmask containing buttons state.
 * @param[in]   has_changed   Bitmask containing buttons
 *                            that have changed their state.
 */
static void button_changed(uint32_t button_state, uint32_t has_changed)
{
	if (IDENTIFY_MODE_BUTTON & has_changed) {
		if (IDENTIFY_MODE_BUTTON & button_state) {
			/* Button changed its state to pressed. */
		} else {
			/* Button changed its state to released. */
			if (was_factory_reset_done()) {
				/* The long press was for Factory Reset. */
				LOG_DBG("After Factory Reset - ignore button release");
			} else {
				/* Button released before Factory Reset. */

				/* Start identification mode. */
				ZB_SCHEDULE_APP_CALLBACK(start_identifying, 0);
			}
		}
	}

	check_factory_reset_button(button_state, has_changed);
}
#endif /* ONKYO_AVR_HAS_BUTTONS */

/**@brief Function for initializing LEDs and Buttons. */
static void configure_gpio(void)
{
	int err;

#if ONKYO_AVR_HAS_BUTTONS
	err = dk_buttons_init(button_changed);
	if (err) {
		LOG_ERR("Cannot init buttons (err: %d)", err);
	}
#endif

	err = dk_leds_init();
	if (err) {
		LOG_ERR("Cannot init LEDs (err: %d)", err);
	}
}

/**@brief Zigbee stack event handler.
 *
 * @param[in]   bufid   Reference to the Zigbee stack buffer
 *                      used to pass signal.
 */
void zboss_signal_handler(zb_bufid_t bufid)
{
	/* Update network status LED. */
	zigbee_led_status_update(bufid, ZIGBEE_NETWORK_STATE_LED);

	/* No application-specific behavior is required.
	 * Call default signal handler.
	 */
	ZB_ERROR_CHECK(zigbee_default_signal_handler(bufid));

	/* All callbacks should either reuse or free passed buffers.
	 * If bufid == 0, the buffer is invalid (not passed).
	 */
	if (bufid) {
		zb_buf_free(bufid);
	}
}

/**@brief Function for handling state reported by the receiver.
 *
 * @param[in]   state   Latest state reported by the receiver.
 */
static void iscp_state_changed(const struct onkyo_iscp_state *state)
{
	k_mutex_lock(&iscp_state_lock, K_FOREVER);
	iscp_state = *state;
	k_mutex_unlock(&iscp_state_lock);

	/* Runs on the ISCP worker, hand the attributes over to the Zigbee thread. */
	(void)ZB_SCHEDULE_APP_CALLBACK(onkyo_apply_receiver_state, 0);
}

/**@brief Request the receiver state that is reflected in the ZCL attributes. */
static void onkyo_receiver_state_query(void)
{
	(void)onkyo_iscp_power_query();
	(void)onkyo_iscp_volume_query();
	(void)onkyo_iscp_mute_query();
	(void)onkyo_iscp_input_query();
}

int main(void)
{
	int err;

	LOG_INF("Starting Zigbee Onkyo TX-SR805 controller V1.1");

	/* Initialize. */
	configure_gpio();
#if ONKYO_AVR_HAS_BUTTONS
	register_factory_reset_button(FACTORY_RESET_BUTTON);
#endif

	/* Register callback for handling ZCL commands. */
	ZB_ZCL_REGISTER_DEVICE_CB(zcl_device_cb);

	/* Register device context (endpoints). */
	ZB_AF_REGISTER_DEVICE_CTX(&onkyo_avr_ctx);

	onkyo_clusters_attr_init();

	/* Register handlers to identify notifications. */
	ZB_AF_SET_IDENTIFY_NOTIFICATION_HANDLER(ONKYO_AVR_ENDPOINT, identify_cb);

	/* Bring up the ISCP link and pick up the state the receiver is in. */
	err = onkyo_iscp_init(iscp_state_changed);
	if (err) {
		LOG_ERR("Cannot initialize the Onkyo serial link (err: %d)", err);
	} else {
		onkyo_receiver_state_query();
	}

	/* Start Zigbee default thread. */
	zigbee_enable();

	LOG_INF("Zigbee Onkyo TX-SR805 controller started");

	return 0;
}
