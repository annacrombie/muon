#include "posix.h"

#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "platform/filesystem.h"
#include "platform/term.h"
#include "log.h"

bool
term_winsize(int fd, uint32_t *height, uint32_t *width)
{
	*height = 24;
	*width = 80;

	if (!fs_is_a_tty_from_fd(fd)) {
		return true;
	}

	struct winsize w = { 0 };
	if (ioctl(fd, TIOCGWINSZ, &w) == -1) {
		return false;
	}

	if (w.ws_row) {
		*height = w.ws_row;
	}
	if (w.ws_col) {
		*width = w.ws_col;
	}
	return true;
}
