/*
 * Copyright (C) 2017 - Julien Desfossez <jdesfossez@efficios.com>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License, version 2.1 only,
 * as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _LGPL_SOURCE
#include <assert.h>
#include <string.h>

#include <lttng/lttng-error.h>
#include <lttng/rotation.h>
#include <lttng/location-internal.h>
#include <lttng/rotate-internal.h>
#include <common/sessiond-comm/sessiond-comm.h>
#include <common/macros.h>

#include "lttng-ctl-helper.h"

struct lttng_rotation_immediate_attr *lttng_rotation_immediate_attr_create(void)
{
	return zmalloc(sizeof(struct lttng_rotation_immediate_attr));
}

struct lttng_rotation_schedule_attr *lttng_rotation_schedule_attr_create(void)
{
	return zmalloc(sizeof(struct lttng_rotation_schedule_attr));
}

void lttng_rotation_immediate_attr_destroy(
		struct lttng_rotation_immediate_attr *attr)
{
	free(attr);
}

void lttng_rotation_schedule_attr_destroy(struct lttng_rotation_schedule_attr *attr)
{
	if (attr) {
		free(attr);
		attr = NULL;
	}
}

enum lttng_rotation_status lttng_rotation_immediate_attr_set_session_name(
		struct lttng_rotation_immediate_attr *attr,
		const char *session_name)
{
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;
	int ret;

	if (!attr || !session_name) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto error;
	}

	ret = lttng_strncpy(attr->session_name, session_name,
			sizeof(attr->session_name));
	if (ret) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto error;
	}

error:
	return status;
}

static
enum lttng_rotation_status ask_rotation_info(
		struct lttng_rotation_handle *rotation_handle,
		struct lttng_rotation_get_info_return **info)
{
	/* lsm.get_rotation_state.rotation_id */
	struct lttcomm_session_msg lsm;
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;
	int ret;

	if (!rotation_handle || !info) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ROTATION_GET_INFO;
	lsm.u.get_rotation_info.rotation_id = rotation_handle->rotation_id;

	ret = lttng_strncpy(lsm.session.name, rotation_handle->session_name,
			sizeof(lsm.session.name));
	if (ret) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto end;
	}

	ret = lttng_ctl_ask_sessiond(&lsm, (void **) info);
	if (ret < 0) {
		status = LTTNG_ROTATION_STATUS_ERROR;
		goto end;
	}
end:
	return status;

}

enum lttng_rotation_status lttng_rotation_schedule_attr_set_session_name(
		struct lttng_rotation_schedule_attr *attr,
		const char *session_name)
{
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;
	int ret;

	if (!attr || !session_name) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto error;
	}

	ret = lttng_strncpy(attr->session_name, session_name,
			sizeof(attr->session_name));
	if (ret) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto error;
	}

error:
	return status;
}

enum lttng_rotation_status lttng_rotation_schedule_attr_set_timer_period(
		struct lttng_rotation_schedule_attr *attr,
		uint64_t timer)
{
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;

	if (!attr) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto end;
	}

	attr->timer_us = timer;
end:
	return status;
}

void lttng_rotation_schedule_attr_set_size(
		struct lttng_rotation_schedule_attr *attr, uint64_t size)
{
	attr->size = size;
}

static
struct lttng_trace_archive_location *
create_trace_archive_location_from_get_info(
		const struct lttng_rotation_get_info_return *info)
{
	struct lttng_trace_archive_location *location;

	switch (info->location_type) {
	case LTTNG_TRACE_ARCHIVE_LOCATION_TYPE_LOCAL:
		location = lttng_trace_archive_location_local_create(
				info->location.local.absolute_path);
		break;
	case LTTNG_TRACE_ARCHIVE_LOCATION_TYPE_RELAY:
		location = lttng_trace_archive_location_relay_create(
				info->location.relay.host,
				info->location.relay.protocol,
				info->location.relay.ports.control,
				info->location.relay.ports.data,
				info->location.relay.relative_path);
		break;
	default:
		location = NULL;
		break;
	}
	return location;
}

enum lttng_rotation_status lttng_rotation_handle_get_state(
		struct lttng_rotation_handle *rotation_handle,
		enum lttng_rotation_state *state)
{
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;
	struct lttng_rotation_get_info_return *info = NULL;

	if (!rotation_handle || !state) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto end;
	}

	status = ask_rotation_info(rotation_handle, &info);
	if (status != LTTNG_ROTATION_STATUS_OK) {
		goto end;
	}

	*state = (enum lttng_rotation_state) info->status;
	if (rotation_handle->archive_location ||
			*state != LTTNG_ROTATION_STATE_COMPLETED) {
		/*
		 * The path is only provided by the sessiond once
		 * the session rotation is completed, but not expired.
		 */
		goto end;
	}

	/*
	 * Cache the location since the rotation may expire before the user
	 * has a chance to query it.
	 */
	rotation_handle->archive_location =
			create_trace_archive_location_from_get_info(info);
	if (!rotation_handle->archive_location) {
		status = LTTNG_ROTATION_STATUS_ERROR;
		goto end;
	}
end:
	free(info);
	return status;
}

enum lttng_rotation_status lttng_rotation_handle_get_archive_location(
		struct lttng_rotation_handle *rotation_handle,
		const struct lttng_trace_archive_location **location)
{
	enum lttng_rotation_status status = LTTNG_ROTATION_STATUS_OK;
	struct lttng_rotation_get_info_return *info = NULL;

	if (!rotation_handle || !location) {
		status = LTTNG_ROTATION_STATUS_INVALID;
		goto end;
	}

	/* Use the cached location we got from a previous query. */
	if (rotation_handle->archive_location) {
		*location = rotation_handle->archive_location;
		goto end;
	}

	status = ask_rotation_info(rotation_handle, &info);
	if (status != LTTNG_ROTATION_STATUS_OK) {
		goto end;
	}

	if ((enum lttng_rotation_state) info->status !=
			LTTNG_ROTATION_STATE_COMPLETED) {
		status = LTTNG_ROTATION_STATUS_UNAVAILABLE;
		goto end;
	}

	rotation_handle->archive_location =
			create_trace_archive_location_from_get_info(info);
	if (!rotation_handle->archive_location) {
		status = LTTNG_ROTATION_STATUS_ERROR;
		goto end;
	}
end:
	free(info);
	return status;
}

void lttng_rotation_handle_destroy(
		struct lttng_rotation_handle *rotation_handle)
{
	if (!rotation_handle) {
		return;
	}
	lttng_trace_archive_location_destroy(rotation_handle->archive_location);
	free(rotation_handle);
}

static
int init_rotation_handle(struct lttng_rotation_handle *rotation_handle,
		struct lttng_rotate_session_return *rotate_return,
		struct lttng_rotation_immediate_attr *attr)
{
	int ret;

	ret = lttng_strncpy(rotation_handle->session_name, attr->session_name,
			sizeof(rotation_handle->session_name));
	if (ret) {
		goto end;
	}

	rotation_handle->rotation_id = rotate_return->rotation_id;
end:
	return ret;
}

/*
 * Rotate the output folder of the session.
 *
 * Return 0 on success else a negative LTTng error code.
 */
int lttng_rotate_session(struct lttng_rotation_immediate_attr *attr,
		struct lttng_rotation_handle **rotation_handle)
{
	struct lttcomm_session_msg lsm;
	struct lttng_rotate_session_return *rotate_return = NULL;
	int ret;

	if (!attr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ROTATE_SESSION;
	lttng_ctl_copy_string(lsm.session.name, attr->session_name,
			sizeof(lsm.session.name));

	ret = lttng_ctl_ask_sessiond(&lsm, (void **) &rotate_return);
	if (ret < 0) {
		*rotation_handle = NULL;
		goto end;
	}

	*rotation_handle = zmalloc(sizeof(struct lttng_rotation_handle));
	if (!*rotation_handle) {
		ret = -LTTNG_ERR_NOMEM;
		goto end;
	}

	init_rotation_handle(*rotation_handle, rotate_return, attr);

	ret = 0;

end:
	free(rotate_return);
	return ret;
}

/*
 * Configure the automatic rotate parameters.
 */
int lttng_rotation_set_schedule(
		struct lttng_rotation_schedule_attr *attr)
{
	struct lttcomm_session_msg lsm;
	int ret;

	if (!attr) {
		ret = -LTTNG_ERR_INVALID;
		goto end;
	}

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ROTATION_SET_SCHEDULE;
	lttng_ctl_copy_string(lsm.session.name, attr->session_name,
			sizeof(lsm.session.name));
	lsm.u.rotate_setup.timer_us = attr->timer_us;
	lsm.u.rotate_setup.size = attr->size;

	ret = lttng_ctl_ask_sessiond(&lsm, NULL);
end:
	return ret;
}

int lttng_rotation_schedule_get_timer_period(const char *session_name,
		uint64_t *rotate_timer)
{
	struct lttcomm_session_msg lsm;
	struct lttng_rotation_schedule_get_timer_period *get_timer = NULL;
	int ret;

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ROTATION_SCHEDULE_GET_TIMER_PERIOD;
	lttng_ctl_copy_string(lsm.session.name, session_name,
			sizeof(lsm.session.name));

	ret = lttng_ctl_ask_sessiond(&lsm, (void **) &get_timer);
	if (ret < 0) {
		ret = -1;
		goto end;
	}

	*rotate_timer = get_timer->rotate_timer;
	ret = 0;
end:
	free(get_timer);
	return ret;
}

int lttng_rotation_schedule_get_size(const char *session_name,
		uint64_t *rotate_size)
{
	struct lttcomm_session_msg lsm;
	struct lttng_rotation_schedule_get_size *get_size = NULL;
	int ret;

	memset(&lsm, 0, sizeof(lsm));
	lsm.cmd_type = LTTNG_ROTATION_SCHEDULE_GET_SIZE;
	lttng_ctl_copy_string(lsm.session.name, session_name,
			sizeof(lsm.session.name));

	ret = lttng_ctl_ask_sessiond(&lsm, (void **) &get_size);
	if (ret < 0) {
		ret = -1;
		goto end;
	}

	*rotate_size = get_size->rotate_size;

	ret = 0;

end:
	free(get_size);
	return ret;
}
