/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2007-2013  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <resolv.h>
#include <netdb.h>

#include "connman.h"

#define RESOLV_CONF_STATEDIR STATEDIR"/resolv.conf"
#define RESOLV_CONF_ETC "/etc/resolv.conf"

#define RESOLVER_FLAG_PUBLIC (1 << 0)

/*
 * Threshold for RDNSS lifetime. Will be used to trigger RS
 * before RDNSS entries actually expire
 */
#define RESOLVER_LIFETIME_REFRESH_THRESHOLD 0.8

struct entry_data {
	int index;
	char *domain;
	char *server;
	int family;
	unsigned int flags;
	unsigned int lifetime;
	guint timeout;
};

static GSList *entry_list = NULL;
static bool dnsproxy_enabled = false;

struct resolvfile_entry {
	int index;
	char *domain;
	char *server;
};

static GList *resolvfile_list = NULL;

static void resolvfile_remove_entries(GList *entries)
{
	GList *list;

	for (list = entries; list; list = list->next) {
		struct resolvfile_entry *entry = list->data;

		resolvfile_list = g_list_remove(resolvfile_list, entry);

		g_free(entry->server);
		g_free(entry->domain);
		g_free(entry);
	}

	g_list_free(entries);
}

static int resolvfile_export(void)
{
	GList *list;
	GString *content;
	int fd, err;
	unsigned int count;
	mode_t old_umask;

	content = g_string_new("# Generated by Connection Manager\n");

	/*
	 * Domains and nameservers are added in reverse so that the most
	 * recently appended entry is the primary one. No more than
	 * MAXDNSRCH/MAXNS entries are used.
	 */

	for (count = 0, list = g_list_last(resolvfile_list);
						list && (count < MAXDNSRCH);
						list = g_list_previous(list)) {
		struct resolvfile_entry *entry = list->data;

		if (!entry->domain)
			continue;

		if (count == 0)
			g_string_append_printf(content, "search ");

		g_string_append_printf(content, "%s ", entry->domain);
		count++;
	}

	if (count)
		g_string_append_printf(content, "\n");

	for (count = 0, list = g_list_last(resolvfile_list);
						list && (count < MAXNS);
						list = g_list_previous(list)) {
		struct resolvfile_entry *entry = list->data;

		if (!entry->server)
			continue;

		g_string_append_printf(content, "nameserver %s\n",
								entry->server);
		count++;
	}

	old_umask = umask(022);

	fd = open(RESOLV_CONF_STATEDIR, O_RDWR | O_CREAT | O_CLOEXEC,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0) {
		connman_warn_once("Cannot create "RESOLV_CONF_STATEDIR" "
			"falling back to "RESOLV_CONF_ETC);

		fd = open(RESOLV_CONF_ETC, O_RDWR | O_CREAT | O_CLOEXEC,
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

		if (fd < 0) {
			err = -errno;
			goto done;
		}
	}

	if (ftruncate(fd, 0) < 0) {
		err = -errno;
		goto failed;
	}

	err = 0;

	if (write(fd, content->str, content->len) < 0)
		err = -errno;

failed:
	close(fd);

done:
	g_string_free(content, TRUE);
	umask(old_umask);

	return err;
}

int __connman_resolvfile_append(int index, const char *domain,
							const char *server)
{
	struct resolvfile_entry *entry;

	DBG("index %d server %s", index, server);

	if (index < 0)
		return -ENOENT;

	entry = g_try_new0(struct resolvfile_entry, 1);
	if (!entry)
		return -ENOMEM;

	entry->index = index;
	entry->domain = g_strdup(domain);
	entry->server = g_strdup(server);

	resolvfile_list = g_list_append(resolvfile_list, entry);

	return resolvfile_export();
}

int __connman_resolvfile_remove(int index, const char *domain,
							const char *server)
{
	GList *list, *matches = NULL;

	DBG("index %d server %s", index, server);

	for (list = resolvfile_list; list; list = g_list_next(list)) {
		struct resolvfile_entry *entry = list->data;

		if (index >= 0 && entry->index != index)
			continue;

		if (domain && g_strcmp0(entry->domain, domain) != 0)
			continue;

		if (g_strcmp0(entry->server, server) != 0)
			continue;

		matches = g_list_append(matches, entry);
	}

	resolvfile_remove_entries(matches);

	return resolvfile_export();
}

void __connman_resolver_append_fallback_nameservers(void)
{
	GSList *list;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index >= 0 && entry->server)
			return;
	}

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index != -1 || !entry->server)
			continue;

		DBG("index %d server %s", entry->index, entry->server);

		if (dnsproxy_enabled) {
			__connman_dnsproxy_append(entry->index, entry->domain,
					entry->server);
		} else {
			__connman_resolvfile_append(entry->index,
					entry->domain, entry->server);
		}
	}
}

static void remove_fallback_nameservers(void)
{
	GSList *list;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index >= 0 || !entry->server)
			continue;

		DBG("index %d server %s", entry->index, entry->server);

		if (dnsproxy_enabled) {
			__connman_dnsproxy_remove(entry->index, entry->domain,
					entry->server);
		} else {
			__connman_resolvfile_remove(entry->index,
					entry->domain, entry->server);
		}
	}
}

static void remove_entries(GSList *entries)
{
	GSList *list;

	for (list = entries; list; list = list->next) {
		struct entry_data *entry = list->data;

		entry_list = g_slist_remove(entry_list, entry);

		if (dnsproxy_enabled) {
			__connman_dnsproxy_remove(entry->index, entry->domain,
							entry->server);
		} else {
			__connman_resolvfile_remove(entry->index, entry->domain,
							entry->server);
		}

		if (entry->timeout)
			g_source_remove(entry->timeout);
		g_free(entry->server);
		g_free(entry->domain);
		g_free(entry);
	}

	g_slist_free(entries);

	__connman_resolver_append_fallback_nameservers();
}

static gboolean resolver_expire_cb(gpointer user_data)
{
	struct entry_data *entry = user_data;
	GSList *list;

	DBG("index %d domain %s server %s",
			entry->index, entry->domain, entry->server);

	list = g_slist_prepend(NULL, entry);

	if (entry->index >= 0) {
		struct connman_service *service;
		service = __connman_service_lookup_from_index(entry->index);
		if (service)
			__connman_service_nameserver_remove(service,
							entry->server, true);
	}

	remove_entries(list);

	return FALSE;
}

static gboolean resolver_refresh_cb(gpointer user_data)
{
	struct entry_data *entry = user_data;
	unsigned int interval;
	struct connman_service *service = NULL;

	/* Round up what we have left from lifetime */
	interval = entry->lifetime *
		(1 - RESOLVER_LIFETIME_REFRESH_THRESHOLD) + 1.0;

	DBG("RDNSS start index %d domain %s "
			"server %s remaining lifetime %d",
			entry->index, entry->domain,
			entry->server, interval);

	entry->timeout = g_timeout_add_seconds(interval,
			resolver_expire_cb, entry);

	if (entry->index >= 0) {
		service = __connman_service_lookup_from_index(entry->index);
		if (service) {
			/*
			 * Send Router Solicitation to refresh RDNSS entries
			 * before their lifetime expires
			 */
			__connman_network_refresh_rs_ipv6(
					__connman_service_get_network(service),
					entry->index);
		}
	}
	return FALSE;
}

static int append_resolver(int index, const char *domain,
				const char *server, unsigned int lifetime,
							unsigned int flags)
{
	struct entry_data *entry;
	unsigned int interval;

	DBG("index %d domain %s server %s lifetime %d flags %d",
				index, domain, server, lifetime, flags);

	if (!server && !domain)
		return -EINVAL;

	entry = g_try_new0(struct entry_data, 1);
	if (!entry)
		return -ENOMEM;

	entry->index = index;
	entry->domain = g_strdup(domain);
	entry->server = g_strdup(server);
	entry->flags = flags;
	entry->lifetime = lifetime;

	if (server)
		entry->family = connman_inet_check_ipaddress(server);

	if (lifetime) {
		interval = lifetime * RESOLVER_LIFETIME_REFRESH_THRESHOLD;

		DBG("RDNSS start index %d domain %s "
				"server %s lifetime threshold %d",
				index, domain, server, interval);

		entry->timeout = g_timeout_add_seconds(interval,
				resolver_refresh_cb, entry);
	}

	if (entry->index >= 0 && entry->server)
		remove_fallback_nameservers();

	entry_list = g_slist_append(entry_list, entry);

	if (dnsproxy_enabled)
		__connman_dnsproxy_append(entry->index, domain, server);
	else
		__connman_resolvfile_append(entry->index, domain, server);

	/*
	 * We update the service only for those nameservers
	 * that are automagically added via netlink (lifetime > 0)
	 */
	if (server && entry->index >= 0 && lifetime) {
		struct connman_service *service;
		service = __connman_service_lookup_from_index(entry->index);
		if (service)
			__connman_service_nameserver_append(service,
							server, true);
	}

	return 0;
}

/**
 * connman_resolver_append:
 * @index: network interface index
 * @domain: domain limitation
 * @server: server address
 *
 * Append resolver server address to current list
 */
int connman_resolver_append(int index, const char *domain,
						const char *server)
{
	GSList *list;

	DBG("index %d domain %s server %s", index, domain, server);

	if (!server && !domain)
		return -EINVAL;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->timeout > 0)
			continue;

		if (entry->index == index &&
				g_strcmp0(entry->domain, domain) == 0 &&
				g_strcmp0(entry->server, server) == 0) {
			if (dnsproxy_enabled)
				__connman_dnsproxy_append(entry->index, domain,
						server);

			return -EEXIST;
		}
	}

	return append_resolver(index, domain, server, 0, 0);
}

/**
 * connman_resolver_append_lifetime:
 * @index: network interface index
 * @domain: domain limitation
 * @server: server address
 * @timeout: server lifetime in seconds
 *
 * Append resolver server address to current list
 */
int connman_resolver_append_lifetime(int index, const char *domain,
				const char *server, unsigned int lifetime)
{
	GSList *list;
	unsigned int interval;

	DBG("index %d domain %s server %s lifetime %d",
				index, domain, server, lifetime);

	if (!server && !domain)
		return -EINVAL;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->timeout == 0 ||
				entry->index != index ||
				g_strcmp0(entry->domain, domain) != 0 ||
				g_strcmp0(entry->server, server) != 0)
			continue;

		g_source_remove(entry->timeout);

		if (lifetime == 0) {
			resolver_expire_cb(entry);
			return 0;
		}

		interval = lifetime * RESOLVER_LIFETIME_REFRESH_THRESHOLD;

		DBG("RDNSS start index %d domain %s "
				"server %s lifetime threshold %d",
				index, domain, server, interval);

		entry->timeout = g_timeout_add_seconds(interval,
				resolver_refresh_cb, entry);
		return 0;
	}

	return append_resolver(index, domain, server, lifetime, 0);
}

/**
 * connman_resolver_remove:
 * @index: network interface index
 * @domain: domain limitation
 * @server: server address
 *
 * Remover resolver server address from current list
 */
int connman_resolver_remove(int index, const char *domain, const char *server)
{
	GSList *list, *matches = NULL;

	DBG("index %d domain %s server %s", index, domain, server);

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index != index)
			continue;

		if (g_strcmp0(entry->domain, domain) != 0)
			continue;

		if (g_strcmp0(entry->server, server) != 0)
			continue;

		matches = g_slist_prepend(matches, entry);
		break;
	}

	if (!matches)
		return -ENOENT;

	remove_entries(matches);

	return 0;
}

/**
 * connman_resolver_remove_all:
 * @index: network interface index
 *
 * Remove all resolver server address for the specified interface index
 */
int connman_resolver_remove_all(int index)
{
	GSList *list, *matches = NULL;

	DBG("index %d", index);

	if (index < 0)
		return -EINVAL;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index != index)
			continue;

		matches = g_slist_prepend(matches, entry);
	}

	if (!matches)
		return -ENOENT;

	remove_entries(matches);

	return 0;
}

int __connman_resolver_redo_servers(int index)
{
	GSList *list;

	if (!dnsproxy_enabled)
		return 0;

	DBG("index %d", index);

	if (index < 0)
		return -EINVAL;

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->timeout == 0 || entry->index != index)
			continue;

		/*
		 * This function must only check IPv6 server addresses so
		 * do not remove IPv4 name servers unnecessarily.
		 */
		if (entry->family != AF_INET6)
			continue;

		/*
		 * We remove the server, and then re-create so that it will
		 * use proper source addresses when sending DNS queries.
		 */
		__connman_dnsproxy_remove(entry->index, entry->domain,
					entry->server);

		__connman_dnsproxy_append(entry->index, entry->domain,
					entry->server);
	}

	/*
	 * We want to re-add all search domains back to search
	 * domain lists as they just got removed for RDNSS IPv6-servers
	 * (above).
	 * Removal of search domains is not necessary
	 * as there can be only one instance of each search domain
	 * in the each dns-servers search domain list.
        */

	for (list = entry_list; list; list = list->next) {
		struct entry_data *entry = list->data;

		if (entry->index != index)
			continue;

		if (entry->server)
			continue;

		__connman_dnsproxy_append(entry->index, entry->domain,
					NULL);
	}

	return 0;
}

static void free_entry(gpointer data)
{
	struct entry_data *entry = data;
	g_free(entry->domain);
	g_free(entry->server);
	g_free(entry);
}

static void free_resolvfile(gpointer data)
{
	struct resolvfile_entry *entry = data;
	g_free(entry->domain);
	g_free(entry->server);
	g_free(entry);
}

int __connman_resolver_init(gboolean dnsproxy)
{
	int i;
	char **ns;

	DBG("dnsproxy %d", dnsproxy);

	if (!dnsproxy)
		return 0;

	if (__connman_dnsproxy_init() < 0) {
		/* Fall back to resolv.conf */
		return 0;
	}

	dnsproxy_enabled = true;

	ns = connman_setting_get_string_list("FallbackNameservers");
	for (i = 0; ns && ns[i]; i += 1) {
		DBG("server %s", ns[i]);
		append_resolver(-1, NULL, ns[i], 0, RESOLVER_FLAG_PUBLIC);
	}

	return 0;
}

void __connman_resolver_cleanup(void)
{
	DBG("");

	if (dnsproxy_enabled)
		__connman_dnsproxy_cleanup();
	else {
		GList *list;
		GSList *slist;

		for (list = resolvfile_list; list; list = g_list_next(list))
			free_resolvfile(list->data);
		g_list_free(resolvfile_list);
		resolvfile_list = NULL;

		for (slist = entry_list; slist; slist = g_slist_next(slist))
			free_entry(slist->data);
		g_slist_free(entry_list);
		entry_list = NULL;
	}
}
