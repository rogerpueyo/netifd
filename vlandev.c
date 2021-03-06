/*
 * netifd - network interface daemon
 * Copyright (C) 2014 Gioacchino Mazzurco <gio@eigenlab.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <string.h>
#include <inttypes.h>

#include "netifd.h"
#include "device.h"
#include "interface.h"
#include "system.h"

enum {
	VLANDEV_ATTR_IFNAME,
	VLANDEV_ATTR_VID,
	VLANDEV_ATTR_INGRESS_QOS_MAPPING,
	VLANDEV_ATTR_EGRESS_QOS_MAPPING,
	__VLANDEV_ATTR_MAX
};

static const struct blobmsg_policy vlandev_attrs[__VLANDEV_ATTR_MAX] = {
	[VLANDEV_ATTR_IFNAME] = { "ifname", BLOBMSG_TYPE_STRING },
	[VLANDEV_ATTR_VID] = { "vid", BLOBMSG_TYPE_INT32 },
	[VLANDEV_ATTR_INGRESS_QOS_MAPPING] = { "ingress_qos_mapping", BLOBMSG_TYPE_ARRAY },
	[VLANDEV_ATTR_EGRESS_QOS_MAPPING] = { "egress_qos_mapping", BLOBMSG_TYPE_ARRAY },
};

static const struct uci_blob_param_list vlandev_attr_list = {
	.n_params = __VLANDEV_ATTR_MAX,
	.params = vlandev_attrs,

	.n_next = 1,
	.next = { &device_attr_list },
};

static struct device_type vlan8021q_device_type;

struct vlandev_device {
	struct device dev;
	struct device_user parent;

	device_state_cb set_state;

	struct blob_attr *config_data;
	struct blob_attr *ifname;
	struct vlandev_config config;
};

static void
vlandev_base_cb(struct device_user *dev, enum device_event ev)
{
	struct vlandev_device *mvdev = container_of(dev, struct vlandev_device, parent);

	switch (ev) {
	case DEV_EVENT_ADD:
		device_set_present(&mvdev->dev, true);
		break;
	case DEV_EVENT_REMOVE:
		device_set_present(&mvdev->dev, false);
		break;
	default:
		return;
	}
}

static int
vlandev_set_down(struct vlandev_device *mvdev)
{
	mvdev->set_state(&mvdev->dev, false);
	system_vlandev_del(&mvdev->dev);
	device_release(&mvdev->parent);

	return 0;
}

static int
vlandev_set_up(struct vlandev_device *mvdev)
{
	int ret;

	ret = device_claim(&mvdev->parent);
	if (ret < 0)
		return ret;

	ret = system_vlandev_add(&mvdev->dev, mvdev->parent.dev, &mvdev->config);
	if (ret < 0)
		goto release;

	ret = mvdev->set_state(&mvdev->dev, true);
	if (ret)
		goto delete;

	return 0;

delete:
	system_vlandev_del(&mvdev->dev);
release:
	device_release(&mvdev->parent);
	return ret;
}

static int
vlandev_set_state(struct device *dev, bool up)
{
	struct vlandev_device *mvdev;

	D(SYSTEM, "vlandev_set_state(%s, %u)\n", dev->ifname, up);

	mvdev = container_of(dev, struct vlandev_device, dev);
	if (up)
		return vlandev_set_up(mvdev);
	else
		return vlandev_set_down(mvdev);
}

static void
vlandev_free(struct device *dev)
{
	struct vlandev_device *mvdev;

	mvdev = container_of(dev, struct vlandev_device, dev);
	device_remove_user(&mvdev->parent);
	free(mvdev->config_data);
	free(mvdev);
}

static void
vlandev_dump_info(struct device *dev, struct blob_buf *b)
{
	struct vlandev_device *mvdev;

	mvdev = container_of(dev, struct vlandev_device, dev);
	blobmsg_add_string(b, "parent", mvdev->parent.dev->ifname);
	system_if_dump_info(dev, b);
}

static void
vlandev_config_init(struct device *dev)
{
	struct vlandev_device *mvdev;
	struct device *basedev = NULL;

	mvdev = container_of(dev, struct vlandev_device, dev);
	if (mvdev->ifname)
		basedev = device_get(blobmsg_data(mvdev->ifname), true);

	device_add_user(&mvdev->parent, basedev);
}

static size_t vlandev_qos_mappings_list_apply(struct ifla_vlan_qos_mapping *qos_mapping, size_t len, struct blob_attr *list)
{
	struct blob_attr *cur;
	int rem, rc;
	int i = 0;

	blobmsg_for_each_attr(cur, list, rem) {
		if (i == len) {
			netifd_log_message(L_WARNING, "parsing failed: too many (>%d) qos mappings\n", len);
			return 0;
		}

		if (blobmsg_type(cur) != BLOBMSG_TYPE_STRING) {
			netifd_log_message(L_WARNING, "parsing failed: qos mapping attr type != string\n");
			return 0;
		}

		if (!blobmsg_check_attr(cur, false)) {
			netifd_log_message(L_WARNING, "parsing failed: qos mapping attr blobmsg_check_attr() failed\n");
			return 0;
		}

		rc = sscanf(blobmsg_data(cur), "%" PRIu32 ":%" PRIu32, &qos_mapping[i].from, &qos_mapping[i].to);
		if (rc != 2) {
			netifd_log_message(L_WARNING, "parsing failed: qos mapping not in form <from_nr>:<to_nr>\n");
			return 0;
		}

		i++;
	}

	return i;
}



static void
vlandev_apply_settings(struct vlandev_device *mvdev, struct blob_attr **tb)
{
	struct vlandev_config *cfg = &mvdev->config;
	struct blob_attr *cur;

	cfg->proto = (mvdev->dev.type == &vlan8021q_device_type) ?
		VLAN_PROTO_8021Q : VLAN_PROTO_8021AD;
	cfg->vid = 1;
	cfg->ingress_qos_mappings_len = 0;
	cfg->egress_qos_mappings_len = 0;

	if ((cur = tb[VLANDEV_ATTR_VID]))
		cfg->vid = (uint16_t) blobmsg_get_u32(cur);

	if ((cur = tb[VLANDEV_ATTR_INGRESS_QOS_MAPPING])) {
		cfg->ingress_qos_mappings_len =
			vlandev_qos_mappings_list_apply(cfg->ingress_qos_mappings,
							ARRAY_SIZE(cfg->ingress_qos_mappings),
							cur);
	}

	if ((cur = tb[VLANDEV_ATTR_EGRESS_QOS_MAPPING])) {
		cfg->egress_qos_mappings_len =
			vlandev_qos_mappings_list_apply(cfg->egress_qos_mappings,
							ARRAY_SIZE(cfg->egress_qos_mappings),
							cur);
	}
}

static enum dev_change_type
vlandev_reload(struct device *dev, struct blob_attr *attr)
{
	struct blob_attr *tb_dev[__DEV_ATTR_MAX];
	struct blob_attr *tb_mv[__VLANDEV_ATTR_MAX];
	enum dev_change_type ret = DEV_CONFIG_APPLIED;
	struct vlandev_device *mvdev;

	mvdev = container_of(dev, struct vlandev_device, dev);
	attr = blob_memdup(attr);

	blobmsg_parse(device_attr_list.params, __DEV_ATTR_MAX, tb_dev,
		blob_data(attr), blob_len(attr));
	blobmsg_parse(vlandev_attrs, __VLANDEV_ATTR_MAX, tb_mv,
		blob_data(attr), blob_len(attr));

	device_init_settings(dev, tb_dev);
	vlandev_apply_settings(mvdev, tb_mv);
	mvdev->ifname = tb_mv[VLANDEV_ATTR_IFNAME];

	if (mvdev->config_data) {
		struct blob_attr *otb_dev[__DEV_ATTR_MAX];
		struct blob_attr *otb_mv[__VLANDEV_ATTR_MAX];

		blobmsg_parse(device_attr_list.params, __DEV_ATTR_MAX, otb_dev,
			blob_data(mvdev->config_data), blob_len(mvdev->config_data));

		if (uci_blob_diff(tb_dev, otb_dev, &device_attr_list, NULL))
		    ret = DEV_CONFIG_RESTART;

		blobmsg_parse(vlandev_attrs, __VLANDEV_ATTR_MAX, otb_mv,
			blob_data(mvdev->config_data), blob_len(mvdev->config_data));

		if (uci_blob_diff(tb_mv, otb_mv, &vlandev_attr_list, NULL))
		    ret = DEV_CONFIG_RESTART;

		vlandev_config_init(dev);
	}

	free(mvdev->config_data);
	mvdev->config_data = attr;
	return ret;
}

static struct device *
vlandev_create(const char *name, struct device_type *devtype,
	       struct blob_attr *attr)
{
	struct vlandev_device *mvdev;
	struct device *dev = NULL;

	mvdev = calloc(1, sizeof(*mvdev));
	if (!mvdev)
		return NULL;

	dev = &mvdev->dev;

	if (device_init(dev, devtype, name) < 0) {
		device_cleanup(dev);
		free(mvdev);
		return NULL;
	}

	dev->config_pending = true;

	mvdev->set_state = dev->set_state;
	dev->set_state = vlandev_set_state;

	dev->hotplug_ops = NULL;
	mvdev->parent.cb = vlandev_base_cb;

	vlandev_reload(dev, attr);

	return dev;
}

static struct device_type vlan8021ad_device_type = {
	.name = "8021ad",
	.config_params = &vlandev_attr_list,
	.create = vlandev_create,
	.config_init = vlandev_config_init,
	.reload = vlandev_reload,
	.free = vlandev_free,
	.dump_info = vlandev_dump_info,
};

static struct device_type vlan8021q_device_type = {
	.name = "8021q",
	.config_params = &vlandev_attr_list,
	.create = vlandev_create,
	.config_init = vlandev_config_init,
	.reload = vlandev_reload,
	.free = vlandev_free,
	.dump_info = vlandev_dump_info,
};

static void __init vlandev_device_type_init(void)
{
	device_type_add(&vlan8021ad_device_type);
	device_type_add(&vlan8021q_device_type);
}
