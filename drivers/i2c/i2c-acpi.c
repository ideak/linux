/*
 * i2c ACPI code
 *
 * Copyright (C) 2014 Intel Corp
 *
 * Author: Lan Tianyu <tianyu.lan@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/acpi.h>

struct i2c_acpi_handler_data {
	struct acpi_connection_info info;
	struct i2c_adapter *adapter;
} __packed;

struct gsb_buffer {
	u8	status;
	u8	len;
	union {
		u16	wdata;
		u8	bdata;
		u8	data[0];
	};
} __packed;

static int acpi_gsb_i2c_read_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[2];
	int ret;
	u8  *buffer;

	buffer = kzalloc(data_len, GFP_KERNEL);
	if (!buffer)
		return AE_NO_MEMORY;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &cmd;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = data_len;
	msgs[1].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		pr_err("i2c read failed\n");
	else
		memcpy(data, buffer, data_len);

	kfree(buffer);
	return ret;
}

static int acpi_gsb_i2c_write_bytes(struct i2c_client *client,
		u8 cmd, u8 *data, u8 data_len)
{

	struct i2c_msg msgs[1];
	u8  *buffer;
	int ret = AE_OK;

	buffer = kzalloc(data_len + 1, GFP_KERNEL);
	if (!buffer)
		return AE_NO_MEMORY;

	buffer[0] = cmd;
	memcpy(buffer + 1, data, data_len);

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = data_len + 1;
	msgs[0].buf = buffer;

	ret = i2c_transfer(client->adapter, msgs, 1);
	if (ret < 0)
		pr_err("i2c write failed\n");

	kfree(buffer);
	return ret;
}

static acpi_status
i2c_acpi_space_handler(u32 function, acpi_physical_address command,
			u32 bits, u64 *value64,
			void *handler_context, void *region_context)
{
	struct gsb_buffer *gsb = (struct gsb_buffer *)value64;
	struct i2c_acpi_handler_data *data = handler_context;
	struct acpi_connection_info *info = &data->info;
	struct acpi_resource_i2c_serialbus *sb;
	struct i2c_adapter *adapter = data->adapter;
	struct i2c_client client;
	struct acpi_resource *ares;
	u32 accessor_type = function >> 16;
	u8 action = function & ACPI_IO_MASK;
	int status;

	acpi_buffer_to_resource(info->connection, info->length, &ares);
	if (!value64 || ares->type != ACPI_RESOURCE_TYPE_SERIAL_BUS)
		return AE_BAD_PARAMETER;

	sb = &ares->data.i2c_serial_bus;
	if (sb->type != ACPI_RESOURCE_SERIAL_TYPE_I2C)
		return AE_BAD_PARAMETER;

	client.adapter = adapter;
	client.addr = sb->slave_address;
	client.flags = 0;

	if (sb->access_mode == ACPI_I2C_10BIT_MODE)
		client.flags |= I2C_CLIENT_TEN;

	switch (accessor_type) {
	case ACPI_GSB_ACCESS_ATTRIB_QUICK:
		if (action == ACPI_READ)
			status = i2c_smbus_quick_read(&client);
		else
			status = i2c_smbus_quick_write(&client);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_SEND_RCV:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte(&client);
			if (status >= 0) {
				gsb->bdata = status;
				status = 0;

			}
		} else
			status = i2c_smbus_write_byte(&client, gsb->bdata);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_BYTE:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_byte_data(&client, command);
			if (status >= 0) {
				gsb->bdata = status;
				status = 0;
			}
		} else
			status = i2c_smbus_write_byte_data(&client, command,
					gsb->bdata);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_WORD:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_word_data(&client, command);
			if (status >= 0) {
				gsb->wdata = status;
				status = 0;
			}
		} else
			status = i2c_smbus_write_word_data(&client, command,
					gsb->wdata);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_BLOCK:
		if (action == ACPI_READ) {
			status = i2c_smbus_read_block_data(&client, command,
					gsb->data);
			if (status >= 0) {
				gsb->len = status;
				status = 0;
			}
		} else
			status = i2c_smbus_write_block_data(&client, command,
					gsb->len, gsb->data);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_MULTIBYTE:
		if (action == ACPI_READ) {
			status = acpi_gsb_i2c_read_bytes(&client, command,
					gsb->data, info->access_length);
			if (status >= 0)
				status = 0;
		} else
			status = acpi_gsb_i2c_write_bytes(&client, command,
					gsb->data, info->access_length);
	break;

	case ACPI_GSB_ACCESS_ATTRIB_WORD_CALL:
		status = i2c_smbus_word_proc_call(&client, command, gsb->wdata);
		if (status >= 0) {
			gsb->wdata = status;
			status = 0;
		}
	break;

	case ACPI_GSB_ACCESS_ATTRIB_BLOCK_CALL:
		status = i2c_smbus_block_proc_call(&client, command, gsb->len,
				gsb->data);
		if (status > 0) {
			gsb->len = status;
			status = 0;
		}
	break;

	default:
		pr_info("protocl(0x%02x) is not supported.", accessor_type);
		return AE_BAD_PARAMETER;
	}

	gsb->status = status;
	return AE_OK;
}


int i2c_acpi_install_space_handler(struct i2c_adapter *adapter)
{
	acpi_handle handle = ACPI_HANDLE(adapter->dev.parent);
	struct i2c_acpi_handler_data *data;
	acpi_status status;

	if (!handle)
		return -EINVAL;

	data = kzalloc(sizeof(struct i2c_acpi_handler_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->adapter = adapter;
	status = acpi_bus_attach_private_data(handle, (void *)data);
	if (ACPI_FAILURE(status))
		return -EFAULT;

	status = acpi_install_address_space_handler(handle,
				ACPI_ADR_SPACE_GSBUS,
				&i2c_acpi_space_handler,
				NULL,
				data);
	if (ACPI_FAILURE(status)) {
		pr_err("Error installing i2c space handler\n");
		acpi_bus_attach_private_data(handle, NULL);
		kfree(data);
		return -EFAULT;
	}

	acpi_walk_dep_device_list();
	return 0;
}

void i2c_acpi_remove_space_handler(struct i2c_adapter *adapter)
{
	acpi_handle handle = ACPI_HANDLE(adapter->dev.parent);
	struct i2c_acpi_handler_data *data;
	acpi_status status;

	if (!handle)
		return;

	acpi_remove_address_space_handler(handle,
				ACPI_ADR_SPACE_GSBUS,
				&i2c_acpi_space_handler);

	status = acpi_bus_get_private_data(handle, (void **)&data);
	if (ACPI_SUCCESS(status))
		kfree(data);
}
