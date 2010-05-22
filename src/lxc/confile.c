/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <dlezcano at fr.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/utsname.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>

#include "parse.h"

#include <lxc/log.h>
#include <lxc/conf.h>

lxc_log_define(lxc_confile, lxc);

static int config_pts(const char *, char *, struct lxc_conf *);
static int config_tty(const char *, char *, struct lxc_conf *);
static int config_cgroup(const char *, char *, struct lxc_conf *);
static int config_mount(const char *, char *, struct lxc_conf *);
static int config_rootfs(const char *, char *, struct lxc_conf *);
static int config_pivotdir(const char *, char *, struct lxc_conf *);
static int config_utsname(const char *, char *, struct lxc_conf *);
static int config_network_type(const char *, char *, struct lxc_conf *);
static int config_network_flags(const char *, char *, struct lxc_conf *);
static int config_network_link(const char *, char *, struct lxc_conf *);
static int config_network_name(const char *, char *, struct lxc_conf *);
static int config_network_hwaddr(const char *, char *, struct lxc_conf *);
static int config_network_mtu(const char *, char *, struct lxc_conf *);
static int config_network_ipv4(const char *, char *, struct lxc_conf *);
static int config_network_ipv6(const char *, char *, struct lxc_conf *);

typedef int (*config_cb)(const char *, char *, struct lxc_conf *);

struct config {
	char *name;
	config_cb cb;
};

static struct config config[] = {

	{ "lxc.pts",            config_pts            },
	{ "lxc.tty",            config_tty            },
	{ "lxc.cgroup",         config_cgroup         },
	{ "lxc.mount",          config_mount          },
	{ "lxc.rootfs",         config_rootfs         },
	{ "lxc.utsname",        config_utsname        },
	{ "lxc.network.type",   config_network_type   },
	{ "lxc.pivotdir",             config_pivotdir             },
	{ "lxc.network.flags",  config_network_flags  },
	{ "lxc.network.link",   config_network_link   },
	{ "lxc.network.name",   config_network_name   },
	{ "lxc.network.hwaddr", config_network_hwaddr },
	{ "lxc.network.mtu",    config_network_mtu    },
	{ "lxc.network.ipv4",   config_network_ipv4   },
	{ "lxc.network.ipv6",   config_network_ipv6   },
};

static const size_t config_size = sizeof(config)/sizeof(struct config);

static struct config *getconfig(const char *key)
{
	int i;

	for (i = 0; i < config_size; i++)
		if (!strncmp(config[i].name, key,
			     strlen(config[i].name)))
			return &config[i];
	return NULL;
}

static int config_network_type(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	struct lxc_list *network = &lxc_conf->network;
	struct lxc_netdev *netdev;
	struct lxc_list *list;

	netdev = malloc(sizeof(*netdev));
	if (!netdev) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	memset(netdev, 0, sizeof(*netdev));
	lxc_list_init(&netdev->ipv4);
	lxc_list_init(&netdev->ipv6);

	list = malloc(sizeof(*list));
	if (!list) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	lxc_list_init(list);
	list->elem = netdev;

	lxc_list_add(network, list);

	if (!strcmp(value, "veth"))
		netdev->type = VETH;
	else if (!strcmp(value, "macvlan"))
		netdev->type = MACVLAN;
	else if (!strcmp(value, "phys"))
		netdev->type = PHYS;
	else if (!strcmp(value, "empty"))
		netdev->type = EMPTY;
	else {
		ERROR("invalid network type %s", value);
		return -1;
	}
	return 0;
}

static int config_ip_prefix(struct in_addr *addr)
{
	if (IN_CLASSA(addr->s_addr))
		return 32 - IN_CLASSA_NSHIFT;
	if (IN_CLASSB(addr->s_addr))
		return 32 - IN_CLASSB_NSHIFT;
	if (IN_CLASSC(addr->s_addr))
		return 32 - IN_CLASSC_NSHIFT;

	return 0;
}

static struct lxc_netdev *network_netdev(const char *key, const char *value,
					 struct lxc_list *network)
{
	struct lxc_netdev *netdev;

	if (lxc_list_empty(network)) {
		ERROR("network is not created for '%s' = '%s' option",
		      key, value);
		return NULL;
	}

	netdev = lxc_list_first_elem(network);
	if (!netdev) {
		ERROR("no network device defined for '%s' = '%s' option",
		      key, value);
		return NULL;
	}

	return netdev;
}

static int network_ifname(char **valuep, char *value)
{
	if (strlen(value) > IFNAMSIZ) {
		ERROR("invalid interface name: %s", value);
		return -1;
	}

	*valuep = strdup(value);
	if (!*valuep) {
		ERROR("failed to dup string '%s'", value);
		return -1;
	}

	return 0;
}

static int config_network_flags(const char *key, char *value,
				struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	netdev->flags |= IFF_UP;

	return 0;
}

static int config_network_link(const char *key, char *value,
			       struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	return network_ifname(&netdev->link, value);
}

static int config_network_name(const char *key, char *value,
			       struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	return network_ifname(&netdev->name, value);
}

static int config_network_hwaddr(const char *key, char *value,
				 struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	netdev->hwaddr = strdup(value);
	if (!netdev->hwaddr) {
		SYSERROR("failed to dup string '%s'", value);
		return -1;
	}

	return 0;
}

static int config_network_mtu(const char *key, char *value,
			      struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	netdev->mtu = strdup(value);
	if (!netdev->mtu) {
		SYSERROR("failed to dup string '%s'", value);
		return -1;
	}

	return 0;
}

static int config_network_ipv4(const char *key, char *value,
			       struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;
	struct lxc_inetdev *inetdev;
	struct lxc_list *list;
	char *cursor, *slash, *addr = NULL, *bcast = NULL, *prefix = NULL;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	inetdev = malloc(sizeof(*inetdev));
	if (!inetdev) {
		SYSERROR("failed to allocate ipv4 address");
		return -1;
	}
	memset(inetdev, 0, sizeof(*inetdev));

	list = malloc(sizeof(*list));
	if (!list) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	lxc_list_init(list);
	list->elem = inetdev;

	addr = value;

	cursor = strstr(addr, " ");
	if (cursor) {
		*cursor = '\0';
		bcast = cursor + 1;
	}

	slash = strstr(addr, "/");
	if (slash) {
		*slash = '\0';
		prefix = slash + 1;
	}

	if (!addr) {
		ERROR("no address specified");
		return -1;
	}

	if (!inet_pton(AF_INET, addr, &inetdev->addr)) {
		SYSERROR("invalid ipv4 address: %s", value);
		return -1;
	}

	if (bcast)
		if (!inet_pton(AF_INET, bcast, &inetdev->bcast)) {
			SYSERROR("invalid ipv4 address: %s", value);
			return -1;
		}

	/* no prefix specified, determine it from the network class */
	inetdev->prefix = prefix ? atoi(prefix) :
		config_ip_prefix(&inetdev->addr);


	lxc_list_add(&netdev->ipv4, list);

	return 0;
}

static int config_network_ipv6(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	struct lxc_netdev *netdev;
	struct lxc_inet6dev *inet6dev;
	struct lxc_list *list;
	char *slash;
	char *netmask;

	netdev = network_netdev(key, value, &lxc_conf->network);
	if (!netdev)
		return -1;

	inet6dev = malloc(sizeof(*inet6dev));
	if (!inet6dev) {
		SYSERROR("failed to allocate ipv6 address");
		return -1;
	}
	memset(inet6dev, 0, sizeof(*inet6dev));

	list = malloc(sizeof(*list));
	if (!list) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	lxc_list_init(list);
	list->elem = inet6dev;

	inet6dev->prefix = 64;
	slash = strstr(value, "/");
	if (slash) {
		*slash = '\0';
		netmask = slash + 1;
		inet6dev->prefix = atoi(netmask);
	}

	if (!inet_pton(AF_INET6, value, &inet6dev->addr)) {
		SYSERROR("invalid ipv6 address: %s", value);
		return -1;
	}

	lxc_list_add(&netdev->ipv6, list);

	return 0;
}

static int config_pts(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	int maxpts = atoi(value);

	lxc_conf->pts = maxpts;

	return 0;
}

static int config_tty(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	int nbtty = atoi(value);

	lxc_conf->tty = nbtty;

	return 0;
}

static int config_cgroup(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	char *token = "lxc.cgroup.";
	char *subkey;
	struct lxc_list *cglist;
	struct lxc_cgroup *cgelem;

	subkey = strstr(key, token);

	if (!subkey)
		return -1;

	if (!strlen(subkey))
		return -1;

	if (strlen(subkey) == strlen(token))
		return -1;

	subkey += strlen(token);

	cglist = malloc(sizeof(*cglist));
	if (!cglist)
		return -1;

	cgelem = malloc(sizeof(*cgelem));
	if (!cgelem) {
		free(cglist);
		return -1;
	}

	cgelem->subsystem = strdup(subkey);
	cgelem->value = strdup(value);
	cglist->elem = cgelem;

	lxc_list_add_tail(&lxc_conf->cgroup, cglist);

	return 0;
}

static int config_fstab(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	if (strlen(value) >= MAXPATHLEN) {
		ERROR("%s path is too long", value);
		return -1;
	}

	lxc_conf->fstab = strdup(value);
	if (!lxc_conf->fstab) {
		SYSERROR("failed to duplicate string %s", value);
		return -1;
	}

	return 0;
}

static int config_mount(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	char *fstab_token = "lxc.mount";
	char *token = "lxc.mount.entry";
	char *subkey;
	char *mntelem;
	struct lxc_list *mntlist;

	subkey = strstr(key, token);

	if (!subkey) {
		subkey = strstr(key, fstab_token);

		if (!subkey)
			return -1;

		return config_fstab(key, value, lxc_conf);
	}

	if (!strlen(subkey))
		return -1;

	mntlist = malloc(sizeof(*mntlist));
	if (!mntlist)
		return -1;

	mntelem = strdup(value);
	mntlist->elem = mntelem;

	lxc_list_add_tail(&lxc_conf->mount_list, mntlist);

	return 0;
}

static int config_rootfs(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	if (strlen(value) >= MAXPATHLEN) {
		ERROR("%s path is too long", value);
		return -1;
	}

	lxc_conf->rootfs = strdup(value);
	if (!lxc_conf->rootfs) {
		SYSERROR("failed to duplicate string %s", value);
		return -1;
	}

	return 0;
}

static int config_pivotdir(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	if (strlen(value) >= MAXPATHLEN) {
		ERROR("%s path is too long", value);
		return -1;
	}

	lxc_conf->pivotdir = strdup(value);
	if (!lxc_conf->pivotdir) {
		SYSERROR("failed to duplicate string %s", value);
		return -1;
	}

	return 0;
}

static int config_utsname(const char *key, char *value, struct lxc_conf *lxc_conf)
{
	struct utsname *utsname;

	utsname = malloc(sizeof(*utsname));
	if (!utsname) {
		SYSERROR("failed to allocate memory");
		return -1;
	}

	if (strlen(value) >= sizeof(utsname->nodename)) {
		ERROR("node name '%s' is too long",
			      utsname->nodename);
		return -1;
	}

	strcpy(utsname->nodename, value);
	lxc_conf->utsname = utsname;

	return 0;
}

static int parse_line(void *buffer, void *data)
{
	struct config *config;
	char *line = buffer;
	char *dot;
	char *key;
	char *value;

	if (lxc_is_line_empty(line))
		return 0;

	line += lxc_char_left_gc(line, strlen(line));
	if (line[0] == '#')
		return 0;

	dot = strstr(line, "=");
	if (!dot) {
		ERROR("invalid configuration line: %s", line);
		return -1;
	}

	*dot = '\0';
	value = dot + 1;

	key = line;
	key[lxc_char_right_gc(key, strlen(key))] = '\0';

	value += lxc_char_left_gc(value, strlen(value));
	value[lxc_char_right_gc(value, strlen(value))] = '\0';

	config = getconfig(key);
	if (!config) {
		ERROR("unknow key %s", key);
		return -1;
	}

	return config->cb(key, value, data);
}

int lxc_config_read(const char *file, struct lxc_conf *conf)
{
	char buffer[MAXPATHLEN];

	return lxc_file_for_each_line(file, parse_line, buffer,
				      sizeof(buffer), conf);
}
