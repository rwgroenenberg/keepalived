/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        Healthcheckrs child process handling.
 *
 * Version:     $Id: check_daemon.c,v 1.1.8 2005/01/25 23:20:11 acassen Exp $
 *
 * Author:      Alexandre Cassen, <acassen@linux-vs.org>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2005 Alexandre Cassen, <acassen@linux-vs.org>
 */

#include "check_daemon.h"
#include "check_parser.h"
#include "check_data.h"
#include "check_ssl.h"
#include "check_api.h"
#include "global_data.h"
#include "ipwrapper.h"
#include "ipvswrapper.h"
#include "pidfile.h"
#include "daemon.h"
#include "list.h"
#include "main.h"
#include "memory.h"
#include "parser.h"
#include "watchdog.h"
#include "vrrp_netlink.h"
#include "vrrp_if.h"

/* Global vars */
static int check_wdog_sd = -1;

/* Healthchecker watchdog data */
static wdog_data check_wdog_data = {
	"Healthcheck Child",
	WDOG_CHECK,
	-1,
	-1,
	start_check_child
};

/* Daemon stop sequence */
static void
stop_check(void)
{
	/* Destroy master thread */
	thread_destroy_master(master);
	free_checkers_queue();
	free_ssl();
	if (!(debug & 16))
		clear_services();
	ipvs_stop();

	/* Stop daemon */
	pidfile_rm(CHECKERS_PID_FILE);

	/* Clean data */
	free_global_data(data);
	free_check_data(check_data);
#ifdef _WITH_VRRP_
	free_interface_queue();
#endif

#ifdef _DEBUG_
	keepalived_free_final("Healthcheck child process");
#endif

	/* free watchdog sd */
	wdog_close(check_wdog_sd, WDOG_CHECK);

	/*
	 * Reached when terminate signal catched.
	 * finally return to parent process.
	 */
	closelog();
	exit(0);
}

/* Daemon init sequence */
static void
start_check(void)
{
	/* Initialize sub-system */
	ipvs_start();
	init_checkers_queue();
#ifdef _WITH_VRRP_
	init_interface_queue();
	kernel_netlink_init();
#endif

	/* Parse configuration file */
	data = alloc_global_data();
	check_data = alloc_check_data();
	init_data(conf_file, check_init_keywords);
	if (!check_data) {
		stop_check();
		return;
	}

	/* Post initializations */
	syslog(LOG_INFO, "Configuration is using : %lu Bytes", mem_allocated);

	/* SSL load static data & initialize common ctx context */
	if (!init_ssl_ctx()) {
		stop_check();
		return;
	}

	/* Processing differential configuration parsing */
	if (reload)
		clear_diff_services();

	/* Initialize IPVS topology */
	if (!init_services()) {
		stop_check();
		return;
	}

	/* Dump configuration */
	if (debug & 4) {
		dump_global_data(data);
		dump_check_data(check_data);
	}

	/* Register healthcheckers software watchdog */
	check_wdog_sd = wdog_init(WDOG_CHECK);

	/* Register checkers thread */
	register_checkers_thread();
}

/* Reload handler */
int
reload_check_thread(thread * thread)
{
	/* set the reloading flag */
	SET_RELOAD;

	/* Destroy master thread */
	thread_destroy_master(master);
	master = thread_make_master();
	free_global_data(data);
	free_checkers_queue();
	free_ssl();
	ipvs_stop();

	/* free watchdog sd */
	wdog_close(check_wdog_sd, WDOG_CHECK);

	/* Save previous conf data */
	old_check_data = check_data;
	check_data = NULL;

	/* Reload the conf */
	mem_allocated = 0;
	start_check();

	/* free backup data */
	free_check_data(old_check_data);
	UNSET_RELOAD;

	return 0;
}

/* Reload handler */
void
sighup_check(int sig)
{
	syslog(LOG_INFO, "Reloading Healthchecker child process on signal");
	thread_add_event(master, reload_check_thread, NULL, 0);
}

/* Terminate handler */
void
sigend_check(int sig)
{
	syslog(LOG_INFO, "Terminating Healthchecker child process on signal");
	if (master)
		thread_add_terminate_event(master);
}

/* CHECK Child signal handling */
void
check_signal_init(void)
{
	signal_set(SIGHUP, sighup_check);
	signal_set(SIGINT, sigend_check);
	signal_set(SIGTERM, sigend_check);
	signal_set(SIGKILL, sigend_check);
	signal_noignore_sigchld();
}

/* Register CHECK thread */
int
start_check_child(void)
{
	pid_t pid;

	/* Dont start if pid is already running */
	if (checkers_running()) {
		syslog(LOG_INFO, "Healthcheck child process already running");
		return -1;
	}

	/* Initialize child process */
	pid = fork();

	if (pid < 0) {
		syslog(LOG_INFO, "Healthcheck child process: fork error(%s)"
			       , strerror(errno));
		return -1;
	} else if (pid) {
		long poll_delay = (wdog_delay_check) ? wdog_delay_check : WATCHDOG_DELAY;
		checkers_child = pid;
		syslog(LOG_INFO, "Starting Healthcheck child process, pid=%d"
			       , pid);
		/* Connect child watchdog */
		check_wdog_data.wdog_pid = pid;
		thread_add_timer(master, wdog_boot_thread, &check_wdog_data,
				 poll_delay);
		return 0;
	}

	/* Opening local CHECK syslog channel */
	openlog(PROG_CHECK, LOG_PID | (debug & 1) ? LOG_CONS : 0, 
		(log_facility==LOG_DAEMON) ? LOG_LOCAL2 : log_facility);

	/* Child process part, write pidfile */
	if (!pidfile_write(CHECKERS_PID_FILE, getpid())) {
		syslog(LOG_INFO, "Healthcheck child process: cannot write pidfile");
		exit(0);
	}

	/* Create the new master thread */
	thread_destroy_master(master);
	master = thread_make_master();

	/* Signal handling initialization */
	check_signal_init();

	/* change to / dir */
	chdir("/");

	/* Set mask */
	umask(0);

	/* Start Healthcheck daemon */
	start_check();

	/* Launch the scheduling I/O multiplexer */
	launch_scheduler();

	/* Finish healthchecker daemon process */
	stop_check();
	exit(0);
}