/*
 * Copyright (c) 2015 Jean-Philippe Ouellet <jpo@vt.edu>
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

#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Highest file descriptor number we'll pass to diff. */
#define DIFF_MAXFD (4)

#define DEFAULT_SHELL "/bin/sh"
#define DIFF_CMD "diff"

struct args {
	size_t capacity;
	size_t argc;
	char **argv;
};

void
args_init(struct args *a, size_t starting_capacity)
{
	assert(starting_capacity > 0);
	a->argv = calloc(starting_capacity, sizeof(*a->argv));
	if (a->argv == NULL)
		err(1, "calloc");

	/* calloc(), so a->argv[0] starts NULL. */
	a->capacity = starting_capacity;
	a->argc = 0;
}

void
args_add(struct args *a, char *s)
{
	char *sdup;

	/*
	 * argv is kept NULL terminated so it can be passed directly
	 * to execv*(), therefore argc can only be up to (capacity-1)
	 * before we must grow.
	 */
	assert(a->argc < a->capacity);
	if (a->argc + 1 == a->capacity) {
		a->capacity *= 2;
		a->argv = realloc(a->argv, a->capacity * sizeof(*a->argv));
		if (a->argv == NULL)
			err(1, "realloc");
		/*
		 * No need for the tmp var realloc dance since we just
		 * bail anyway, and also no need to init the new area.
		 */
	}

	sdup = strdup(s);
	if (sdup == NULL)
		err(1, "malloc");

	a->argc++;
	a->argv[a->argc - 1] = sdup;
	a->argv[a->argc] = NULL;
}

void
closed_reader(int fd)
{
	int pipes[2];
	if (pipe(pipes) == -1)
		err(1, "pipe");
	close(pipes[1]);
	if (fd != pipes[0]) {
		if (dup2(pipes[0], fd) == -1)
			err(1, "dup2");
		close(pipes[0]);
	}
}

void
clear_cloexec(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFD, 0);
	if (flags == -1)
		err(1, "fcntl F_GETFD");

	flags &= ~FD_CLOEXEC;

	if (fcntl(fd, F_SETFD, flags) == -1)
		err(1, "fcntl F_SETFD");
}

int
xdup_ge(int fd, int ge)
{
	/*
	 * Like a cross between dup()/dup3(..., O_CLOEXEC), but
	 * specifying a minimum fd instead of specific fd. Used
	 * to avoid the (slim) possibility of:
	 *	dup2(pa[0], 3);
	 *	dup2(pb[0], 4);
	 * where pb[0] was originally 3.
	 */
	int newfd;
	newfd = fcntl(fd, F_DUPFD_CLOEXEC, ge);
	if (newfd == -1)
		err(1, "fcntl F_DUPFD");
	return newfd;
}

void
producer(const char *shell, const char *cmd, int pout)
{
	if (dup2(pout, 1) == -1)
		err(1, "dup2");
	clear_cloexec(1);
	closed_reader(0);
	execl(shell, shell, "-c", cmd, NULL);
	err(1, "exec");
}

int
main(int argc, char *argv[])
{
	struct args a;
	char *cmda, *cmdb, *shell;
	pid_t ca, cb, cdiff;
	int i, p[2], pa[2], pb[2], status;

	args_init(&a, 2); /* Could be smarter about size. */
	args_add(&a, DIFF_CMD);

	if (argc < 3) {
		fprintf(stderr, "Usage: %s [diff args] "
		    "'shell command 1' 'shell command 2'\n",
		    argv[0]); /* getprogname() is not portable. */
		exit(2);
	} else if (argc == 3) {
		/* If no diff args are specified, default to -u. */
		args_add(&a, "-u");
	} else /* argc > 3 */ {
		/* Pass all other args on to diff as-is. */
		for (i = 1; i < argc - 2; i++)
			args_add(&a, argv[i]);
	}

	cmda = argv[argc - 2];
	cmdb = argv[argc - 1];

	shell = secure_getenv("SHELL");
	if (shell == NULL)
		shell = DEFAULT_SHELL;

	/*
	 * Safely establish a pair of O_CLOEXEC'd pipes, with all fd
	 * numbers higher than the max one we will pass to diff.
	 */
	if (pipe(p) == -1)
		err(1, "pipe2");
	pa[0] = xdup_ge(p[0], DIFF_MAXFD + 1);
	pa[1] = xdup_ge(p[1], DIFF_MAXFD + 1);
	close(p[0]);
	close(p[1]);

	if (pipe(p) == -1)
		err(1, "pipe2");
	pb[0] = xdup_ge(p[0], DIFF_MAXFD + 1);
	pb[1] = xdup_ge(p[1], DIFF_MAXFD + 1);
	close(p[0]);
	close(p[1]);

	switch ((ca = fork())) {
	case -1:
		err(1, "fork");
	case 0:
		producer(shell, cmda, pa[1]);
		/* NOTREACHED */
	}

	switch ((cb = fork())) {
	case -1:
		err(1, "fork");
	case 0:
		producer(shell, cmdb, pb[1]);
		/* NOTREACHED */
	}

	switch ((cdiff = fork())) {
	case -1:
		err(1, "fork");
	case 0:
		dup2(pa[0], 3);
		dup2(pb[0], 4);
		clear_cloexec(3);
		clear_cloexec(4);
		closed_reader(0);

#ifdef FD_DEBUG
		execlp("ls", "ls", "-al", "/proc/self/fd", NULL);
#else
		args_add(&a, "--label");
		args_add(&a, cmda);
		args_add(&a, "/proc/self/fd/3");

		args_add(&a, "--label");
		args_add(&a, cmdb);
		args_add(&a, "/proc/self/fd/4");

		execvp(DIFF_CMD, a.argv);
#endif
		err(1, "exec");
	}

	close(pa[0]);
	close(pa[1]);
	close(pb[0]);
	close(pb[1]);

	/* TODO: On SIGINT, SIGKILL children & exit. */

	/* TODO: Better waiting. */
	waitpid(cdiff, &status, 0);

	return 0;
}
