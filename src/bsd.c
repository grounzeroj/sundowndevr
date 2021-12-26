/*
 * Copyright (c) 2013 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/event.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <time.h>
#include <regex.h>
#include <zlib.h>
#include <unistd.h>

#include "spdy.h"
#include "kore.h"
#include "http.h"

#define KQUEUE_EVENTS		500
static int			kfd = -1;
static struct kevent		*events;
static int			nchanges;
static struct kevent		*changelist;

void
kore_init(void)
{
	cpu_count = 0;
}

void
kore_worker_init(void)
{
	u_int16_t		i;

	if (worker_count == 0)
		fatal("no workers specified");

	kore_debug("kore_worker_init(): starting %d workers", worker_count);

	TAILQ_INIT(&kore_workers);
	for (i = 0; i < worker_count; i++)
		kore_worker_spawn(0);
}

void
kore_worker_wait(int final)
{
	pid_t			pid;
	int			status;
	struct kore_worker	k, *kw, *next;

	if (final)
		pid = waitpid(WAIT_ANY, &status, 0);
	else
		pid = waitpid(WAIT_ANY, &status, WNOHANG);

	if (pid == -1) {
		kore_debug("waitpid(): %s", errno_s);
		return;
	}

	if (pid == 0)
		return;

	for (kw = TAILQ_FIRST(&kore_workers); kw != NULL; kw = next) {
		next = TAILQ_NEXT(kw, list);
		if (kw->pid != pid)
			continue;

		k = *kw;
		TAILQ_REMOVE(&kore_workers, kw, list);
		kore_log(LOG_NOTICE, "worker %d (%d)-> status %d",
		    kw->id, pid, status);
		free(kw);

		if (final)
			continue;

		if (WEXITSTATUS(status) || WTERMSIG(status) ||
		    WCOREDUMP(status)) {
			kore_log(LOG_NOTICE,
			    "worker %d (pid: %d) gone, respawning new one",
			    k.id, k.pid);
			kore_worker_spawn(0);
		}
	}
}

void
kore_worker_setcpu(struct kore_worker *kw)
{
}

void
kore_event_init(void)
{
	if ((kfd = kqueue()) == -1)
		fatal("kqueue(): %s", errno_s);

	nchanges = 0;
	events = kore_calloc(KQUEUE_EVENTS, sizeof(struct kevent));
	changelist = kore_calloc(KQUEUE_EVENTS, sizeof(struct kevent));
	kore_event_schedule(server.fd, EVFILT_READ, EV_ADD, &server);
}

void
kore_event_wait(int quit)
{
	struct connection	*c;
	int			n, i, *fd;

	n = kevent(kfd, changelist, nchanges, events, KQUEUE_EVENTS, NULL);
	if (n == -1) {
		if (errno == EINTR)
			return;
		fatal("kevent(): %s", errno_s);
	}

	nchanges = 0;
	if (n > 0)
		kore_debug("main(): %d sockets available", n);

	for (i = 0; i < n; i++) {
		fd = (int *)events[i].udata;

		if (events[i].flags & EV_EOF ||
		    events[i].flags & EV_ERROR) {
			if (*fd == server.fd)
				fatal("error on server socket");

			c = (struct connection *)events[i].udata;
			kore_server_disconnect(c);
			continue;
		}

		if (*fd == server.fd) {
			if (!quit) {
				kore_server_accept(&server, &c);
				if (c == NULL)
					continue;

				kore_event_schedule(c->fd, EVFILT_READ,
				    EV_ADD, c);
				kore_event_schedule(c->fd, EVFILT_WRITE,
				    EV_ADD | EV_ONESHOT, c);
			}
		} else {
			c = (struct connection *)events[i].udata;
			if (events[i].filter == EVFILT_READ)
				c->flags |= CONN_READ_POSSIBLE;
			if (events[i].filter == EVFILT_WRITE)
				c->flags |= CONN_WRITE_POSSIBLE;

			if (!kore_connection_handle(c)) {
				kore_server_disconnect(c);
			} else {
				if (!TAILQ_EMPTY(&(c->send_queue))) {
					kore_event_schedule(c->fd, EVFILT_WRITE,
					    EV_ADD | EV_ONESHOT, c);
				}
			}
		}
	}
}

void
kore_event_schedule(int fd, int type, int flags, void *data)
{
	if (nchanges >= KQUEUE_EVENTS) {
		kore_log(LOG_WARNING, "cannot schedule %d (%d) on %d",
		    type, flags, fd);
	} else {
		EV_SET(&changelist[nchanges], fd, type, flags, 0, 0, data);
		nchanges++;
	}
}

void
kore_set_proctitle(char *title)
{
	setproctitle("%s", title);
}
