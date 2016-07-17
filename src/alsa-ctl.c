/*
 * bluealsa-ctl - alsa-ctl.c
 * Copyright (c) 2016 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <alsa/asoundlib.h>
#include <alsa/control_external.h>

#include "ctl.h"


struct bluealsa_ctl {
	snd_ctl_ext_t ext;

	/* bluealsa socket */
	int fd;

	/* list of all currently available transports */
	struct ctl_transport *transports;
	unsigned int transports_count;

};


static void bluealsa_close(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;
	close(ctl->fd);
	free(ctl->transports);
	free(ctl);
}

static int bluealsa_elem_count(snd_ctl_ext_t *ext) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	struct ctl_request req = {
		.command = CTL_COMMAND_LIST_TRANSPORTS,
	};

	send(ctl->fd, &req, sizeof(req), MSG_NOSIGNAL);

	struct ctl_transport transport;
	int i = 0;

	while (recv(ctl->fd, &transport, sizeof(transport), 0) == sizeof(transport)) {
		ctl->transports = realloc(ctl->transports, (i + 1) * sizeof(*ctl->transports));
		memcpy(&ctl->transports[i], &transport, sizeof(*ctl->transports));
		i++;
	}

	ctl->transports_count = i;
	return i;
}

static int bluealsa_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	if (offset > ctl->transports_count)
		return -EINVAL;

	struct ctl_transport *transport = &ctl->transports[offset];

	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, transport->name);

	return 0;
}

static snd_ctl_ext_key_t bluealsa_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id) {
	struct bluealsa_ctl *ctl = (struct bluealsa_ctl *)ext->private_data;

	unsigned int count = ctl->transports_count;
	unsigned int i, numid;
	const char *name;

	numid = snd_ctl_elem_id_get_numid(id);
	if (numid > 0 && numid <= count)
		return numid - 1;

	name = snd_ctl_elem_id_get_name(id);
	for (i = 0; i < count; i++)
		if (strcmp(name, ctl->transports[i].name) == 0)
			return i;

	return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int bluealsa_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		int *type, unsigned int *acc, unsigned int *count) {
	*type = SND_CTL_ELEM_TYPE_INTEGER;
	*acc = SND_CTL_EXT_ACCESS_READWRITE;
	*count = 1;
	return 0;
}

static int bluealsa_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
		long *imin, long *imax, long *istep) {
	*istep = 1;
	*imin = 0;
	*imax = 10;
	return 0;
}

static int bluealsa_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	*value = 7;
	return 0;
}

static int bluealsa_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
	return 0;
}

static int bluealsa_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask) {
	return 0;
}

static const snd_ctl_ext_callback_t bluealsa_snd_ctl_ext_callback = {
	.close = bluealsa_close,
	.elem_count = bluealsa_elem_count,
	.elem_list = bluealsa_elem_list,
	.find_elem = bluealsa_find_elem,
	.get_attribute = bluealsa_get_attribute,
	.get_integer_info = bluealsa_get_integer_info,
	.read_integer = bluealsa_read_integer,
	.write_integer = bluealsa_write_integer,
	.read_event = bluealsa_read_event,
};

SND_CTL_PLUGIN_DEFINE_FUNC(bluealsa) {
	(void)root;

	snd_config_iterator_t i, next;
	const char *interface = "hci0";
	struct bluealsa_ctl *ctl;
	int ret;

	snd_config_for_each(i, next, conf) {
		snd_config_t *n = snd_config_iterator_entry(i);

		const char *id;
		if (snd_config_get_id(n, &id) < 0)
			continue;

		if (strcmp(id, "comment") == 0 ||
				strcmp(id, "type") == 0 ||
				strcmp(id, "hint") == 0)
			continue;

		if (strcmp(id, "interface") == 0) {
			if (snd_config_get_string(n, &interface) < 0) {
				SNDERR("Invalid type for %s", id);
				return -EINVAL;
			}
			continue;
		}

		SNDERR("Unknown field %s", id);
		return -EINVAL;
	}

	if ((ctl = calloc(1, sizeof(*ctl))) == NULL)
		return -ENOMEM;

	struct sockaddr_un saddr = { .sun_family = AF_UNIX };
	snprintf(saddr.sun_path, sizeof(saddr.sun_path) - 1,
			BLUEALSA_RUN_STATE_DIR "/%s", interface);

	if ((ctl->fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) == -1) {
		ret = -errno;
		goto fail;
	}

	if (connect(ctl->fd, (struct sockaddr *)(&saddr), sizeof(saddr)) == -1) {
		SNDERR("BlueALSA connection failed: %s", strerror(errno));
		ret = -ENODEV;
		goto fail;
	}

	ctl->ext.version = SND_CTL_EXT_VERSION;
	ctl->ext.card_idx = -1;
	strncpy(ctl->ext.id, "bluealsa", sizeof(ctl->ext.id) - 1);
	strncpy(ctl->ext.driver, "BlueALSA", sizeof(ctl->ext.driver) - 1);
	strncpy(ctl->ext.name, "BlueALSA", sizeof(ctl->ext.name) - 1);
	strncpy(ctl->ext.longname, "ALSA Bluetooth Controller", sizeof(ctl->ext.longname) - 1);
	strncpy(ctl->ext.mixername, "BlueALSA Plugin", sizeof(ctl->ext.mixername) - 1);

	ctl->ext.callback = &bluealsa_snd_ctl_ext_callback;
	ctl->ext.private_data = ctl;

	if ((ret = snd_ctl_ext_create(&ctl->ext, name, mode)) < 0)
		goto fail;

	*handlep = ctl->ext.handle;
	return 0;

fail:
	if (ctl->fd != -1)
		close(ctl->fd);
	free(ctl);
	return ret;
}

SND_CTL_PLUGIN_SYMBOL(bluealsa);
