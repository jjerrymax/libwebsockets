/*
 * libwebsockets web server application
 *
 * Copyright (C) 2010-2016 Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * The person who associated a work with this deed has dedicated
 * the work to the public domain by waiving all of his or her rights
 * to the work worldwide under copyright law, including all related
 * and neighboring rights, to the extent allowed by law. You can copy,
 * modify, distribute and perform the work, even for commercial purposes,
 * all without asking permission.
 *
 * The test apps are intended to be adapted for use in your code, which
 * may be proprietary.	So unlike the library itself, they are licensed
 * Public Domain.
 */
#include "lws_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#ifndef _WIN32
#include <dirent.h>
#include <syslog.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <io.h>
#include "gettimeofday.h"
#endif

#include "../lib/libwebsockets.h"

#include <uv.h>

static struct lws_context *context;
static char config_dir[128];
static int opts = 0;
static uv_loop_t loop;
static uv_signal_t signal_outer;

#define LWSWS_CONFIG_STRING_SIZE (32 * 1024)

static const struct lws_extension exts[] = {
	{
		"permessage-deflate",
		lws_extension_callback_pm_deflate,
		"permessage-deflate"
	},
	{ NULL, NULL, NULL /* terminator */ }
};

static const char * const plugin_dirs[] = {
	INSTALL_DATADIR"/libwebsockets-test-server/plugins/",
	NULL
};

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",	required_argument,	NULL, 'd' },
	{ "configdir",  required_argument,	NULL, 'c' },
#ifndef LWS_NO_DAEMONIZE
	{ "daemonize", 	no_argument,		NULL, 'D' },
#endif
	{ NULL, 0, 0, 0 }
};

static int
context_creation(void);

static int
lwsws_deprecate_context(void)
{
	struct lws_context *old = context;

	if (context_creation()) {
		lwsl_err("Reloading configuration failed\n");
		return 0;
	}
	lws_context_attach_deprecated(context, old);
	lwsl_notice("Configuration reloaded\n");

	return 0;
}

void signal_cb(uv_signal_t *watcher, int signum)
{
	switch (watcher->signum) {
	case SIGTERM:
	case SIGINT:
		break;

	case SIGHUP:
		if (lws_context_is_deprecated(context))
			return;
		lwsl_notice("Reloading configuration\n");
		lws_context_deprecate(context, lwsws_deprecate_context);
		return;

	default:
		signal(SIGABRT, SIG_DFL);
		abort();
		break;
	}
	lwsl_err("Signal %d caught\n", watcher->signum);
	lws_libuv_stop(context);
}

static int
context_creation(void)
{
	int cs_len = LWSWS_CONFIG_STRING_SIZE - 1;
	struct lws_context_creation_info info;
	char *cs, *config_strings;

	cs = config_strings = malloc(LWSWS_CONFIG_STRING_SIZE);
	if (!config_strings) {
		lwsl_err("Unable to allocate config strings heap\n");
		return -1;
	}

	memset(&info, 0, sizeof(info));

	info.external_baggage_free_on_destroy = config_strings;
	info.max_http_header_pool = 16;
	info.options = opts | LWS_SERVER_OPTION_VALIDATE_UTF8 |
			      LWS_SERVER_OPTION_EXPLICIT_VHOSTS |
			      LWS_SERVER_OPTION_LIBUV;

	info.plugin_dirs = plugin_dirs;
	lwsl_notice("Using config dir: \"%s\"\n", config_dir);

	/*
	 *  first go through the config for creating the outer context
	 */
	if (lwsws_get_config_globals(&info, config_dir, &cs, &cs_len))
		goto init_failed;

	context = lws_create_context(&info);
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		goto init_failed;
	}

	lws_uv_sigint_cfg(context, 1, signal_cb);
	lws_uv_initloop(context, &loop, 0);

	/*
	 * then create the vhosts... protocols are entirely coming from
	 * plugins, so we leave it NULL
	 */

	info.extensions = exts;

	if (lwsws_get_config_vhosts(context, &info, config_dir,
				    &cs, &cs_len))
		return 1;

	return 0;

init_failed:
	free(config_strings);

	return 1;
}

#if 0
static void lws_uv_walk_cb(uv_handle_t *handle, void *arg)
{
	uv_close(handle, NULL);
}
#endif

int main(int argc, char **argv)
{
	int n = 0, debug_level = 7;
#ifndef _WIN32
	int syslog_options = LOG_PID | LOG_PERROR;
#endif
#ifndef LWS_NO_DAEMONIZE
 	int daemonize = 0;
#endif

	strcpy(config_dir, "/etc/lwsws");
	while (n >= 0) {
		n = getopt_long(argc, argv, "hd:c:D", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
#ifndef LWS_NO_DAEMONIZE
		case 'D':
			daemonize = 1;
			#ifndef _WIN32
			syslog_options &= ~LOG_PERROR;
			#endif
			printf("Daemonizing...\n");
			break;
#endif
		case 'd':
			debug_level = atoi(optarg);
			break;
		case 'c':
			strncpy(config_dir, optarg, sizeof(config_dir) - 1);
			config_dir[sizeof(config_dir) - 1] = '\0';
			break;
		case 'h':
			fprintf(stderr, "Usage: lwsws [-c <config dir>] "
					"[-d <log bitfield>] [-D] [--help]\n");
			exit(1);
		}
	}

#if !defined(LWS_NO_DAEMONIZE) && !defined(WIN32)
	/*
	 * normally lock path would be /var/lock/lwsts or similar, to
	 * simplify getting started without having to take care about
	 * permissions or running as root, set to /tmp/.lwsts-lock
	 */
	if (daemonize && lws_daemonize("/tmp/.lwsts-lock")) {
		fprintf(stderr, "Failed to daemonize\n");
		return 10;
	}
	if (daemonize)
		lwsl_notice("Daemonized\n");
#endif

#ifndef _WIN32
	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsws", syslog_options, LOG_DAEMON);
#endif

	lws_set_log_level(debug_level, lwsl_emit_syslog);

	lwsl_notice("lwsws libwebsockets web server - license CC0 + LGPL2.1\n");
	lwsl_notice("(C) Copyright 2010-2016 Andy Green <andy@warmcat.com>\n");

#if (UV_VERSION_MAJOR > 0) // Travis...
	uv_loop_init(&loop);
#else
	fprintf(stderr, "Your libuv is too old!\n");
	return 0;
#endif
	uv_signal_init(&loop, &signal_outer);
	uv_signal_start(&signal_outer, signal_cb, SIGINT);
	uv_signal_start(&signal_outer, signal_cb, SIGHUP);

	if (context_creation()) {
		lwsl_err("Context creation failed\n");
		return 1;
	}

	lws_libuv_run(context, 0);

	uv_signal_stop(&signal_outer);
	lws_context_destroy(context);

#if (UV_VERSION_MAJOR > 0) // Travis...
	n = 0;
	while (n++ < 1024 && uv_loop_close(&loop))
		uv_run(&loop, UV_RUN_NOWAIT);
#endif

	lws_context_destroy2(context);

	fprintf(stderr, "lwsws exited cleanly\n");

#ifndef _WIN32
	closelog();
#endif

	return 0;
}
