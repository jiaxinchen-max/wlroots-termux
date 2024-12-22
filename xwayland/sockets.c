#undef _POSIX_C_SOURCE
#define _XOPEN_SOURCE 700 // for S_ISVTX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "sockets.h"

static const char lock_fmt[] = "/tmp/.X%d-lock";
static const char socket_dir[] = "/tmp/.X11-unix";
static const char socket_fmt[] = "/tmp/.X11-unix/X%d";

bool set_cloexec(int fd, bool cloexec) {
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		wlr_log_errno(WLR_ERROR, "fcntl failed");
		return false;
	}
	if (cloexec) {
		flags = flags | FD_CLOEXEC;
	} else {
		flags = flags & ~FD_CLOEXEC;
	}
	if (fcntl(fd, F_SETFD, flags) == -1) {
		wlr_log_errno(WLR_ERROR, "fcntl failed");
		return false;
	}
	return true;
}

static bool check_socket_dir(void) {
	struct stat buf;

	if (lstat(socket_dir, &buf)) {
		wlr_log_errno(WLR_ERROR, "Failed to stat %s", socket_dir);
		return false;
	}
	if (!(buf.st_mode & S_IFDIR)) {
		wlr_log(WLR_ERROR, "%s is not a directory", socket_dir);
		return false;
	}
	if (!((buf.st_uid == 0) || (buf.st_uid == getuid()))) {
		wlr_log(WLR_ERROR, "%s not owned by root or us", socket_dir);
		return false;
	}
	if (!(buf.st_mode & S_ISVTX)) {
		/* we can deal with no sticky bit... */
		if ((buf.st_mode & (S_IWGRP | S_IWOTH))) {
			/* but not if other users can mess with our sockets */
			wlr_log(WLR_ERROR, "sticky bit not set on %s", socket_dir);
			return false;
		}
	}
	return true;
}

static bool setup_socket_dir(void) {
	mode_t dir_mode = 01777;

	if (mkdir(socket_dir, dir_mode) != 0) {
		if (errno == EEXIST) {
			return check_socket_dir();
		}
		wlr_log_errno(WLR_ERROR, "Unable to mkdir %s", socket_dir);
		return false;
	}

	wlr_log(WLR_INFO, "Created %s ourselves -- other users will "
		"be unable to create X11 UNIX sockets of their own",
		socket_dir);

	// The mode passed to mkdir() is affected by umask, so set it again
	if (chmod(socket_dir, dir_mode) != 0) {
		wlr_log_errno(WLR_ERROR, "Failed to chmod %s", socket_dir);
		return false;
	}

	return true;
}

static int open_socket(int display) {
	if (!setup_socket_dir()) {
		return -1;
	}

	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	size_t path_size = snprintf(addr.sun_path, sizeof(addr.sun_path), socket_fmt, display);

	socklen_t size = offsetof(struct sockaddr_un, sun_path) + path_size + 1;

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create socket %s", addr.sun_path);
		return -1;
	}
	if (!set_cloexec(fd, true)) {
		close(fd);
		return -1;
	}

	int rc;
	unlink(addr.sun_path);
	if (bind(fd, (struct sockaddr *)&addr, size) < 0) {
		rc = errno;
		wlr_log_errno(WLR_ERROR, "Failed to bind socket %s", addr.sun_path);
		goto cleanup;
	}
	if (listen(fd, 1) < 0) {
		rc = errno;
		wlr_log_errno(WLR_ERROR, "Failed to listen to socket %s", addr.sun_path);
		goto cleanup;
	}

	return fd;

cleanup:
	close(fd);
	unlink(addr.sun_path);
	errno = rc;
	return -1;
}

void unlink_display_sockets(int display) {
	char path[64];

	snprintf(path, sizeof(path), socket_fmt, display);
	unlink(path);

	snprintf(path, sizeof(path), lock_fmt, display);
	unlink(path);
}

int open_display_sockets(int *sock) {
	int lock_fd, display;
	char lock_name[64];

	for (display = 0; display <= 32; display++) {
		snprintf(lock_name, sizeof(lock_name), lock_fmt, display);
		if ((lock_fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444)) >= 0) {
			*sock = open_socket(display);
			if (*sock < 0) {
				unlink(lock_name);
				close(lock_fd);
				continue;
			}
			char pid[12];
			snprintf(pid, sizeof(pid), "%10d", getpid());
			if (write(lock_fd, pid, sizeof(pid) - 1) != sizeof(pid) - 1) {
				unlink(lock_name);
				close(lock_fd);
				continue;
			}
			close(lock_fd);
			break;
		}

		if ((lock_fd = open(lock_name, O_RDONLY | O_CLOEXEC)) < 0) {
			continue;
		}

		char pid[12] = { 0 }, *end_pid;
		ssize_t bytes = read(lock_fd, pid, sizeof(pid) - 1);
		close(lock_fd);

		if (bytes != sizeof(pid) - 1) {
			continue;
		}
		long int read_pid;
		read_pid = strtol(pid, &end_pid, 10);
		if (read_pid < 0 || read_pid > INT32_MAX || end_pid != pid + sizeof(pid) - 2) {
			continue;
		}
		errno = 0;
		if (kill((pid_t)read_pid, 0) != 0 && errno == ESRCH) {
			if (unlink(lock_name) != 0) {
				continue;
			}
			// retry
			display--;
			continue;
		}
	}

	if (display > 32) {
		wlr_log(WLR_ERROR, "No display available in the first 33");
		return -1;
	}

	return display;
}
