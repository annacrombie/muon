#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>

#include "log.h"
#include "platform/mem.h"
#include "platform/filesystem.h"
#include "platform/windows/win32_error.h"

bool
fs_exists(const char *path)
{
	HANDLE h;

	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_ARCHIVE | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	CloseHandle(h);

 	return true;
}

bool
fs_symlink_exists(const char *path)
{
	return false;
	(void)path;
}

bool
fs_file_exists(const char *path)
{
	BY_HANDLE_FILE_INFORMATION fi;
	HANDLE h;

	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_ARCHIVE, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (!GetFileInformationByHandle(h, &fi)) {
		return false;
	}

	CloseHandle(h);

	return (fi.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) == FILE_ATTRIBUTE_ARCHIVE;
}

bool
fs_exe_exists(const char *path)
{
	DWORD type;

	if (!GetBinaryType(path, &type)) {
		return false;
	}

	return (type == SCS_64BIT_BINARY) || (type == SCS_32BIT_BINARY);
}

bool
fs_dir_exists(const char *path)
{
	BY_HANDLE_FILE_INFORMATION fi;
	HANDLE h;

	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	if (!GetFileInformationByHandle(h, &fi)) {
		return false;
	}

	CloseHandle(h);

	return (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY;
}

bool
fs_mkdir(const char *path)
{
	if (!CreateDirectory(path, NULL)) {
		LOG_E("failed to create directory %s: %s", path, win32_error());
		return false;
	}

	return true;
}

bool
fs_copy_file(const char *src, const char *dest)
{
	if (!CopyFile(src, dest, FALSE)) {
		LOG_E("failed to copy file %s: %s", src, win32_error());
		return false;
	}

	return true;
}

bool
fs_dir_foreach(const char *path, void *_ctx, fs_dir_foreach_cb cb)
{
	HANDLE h;
	char *filter;
	WIN32_FIND_DATA fd;
	size_t len;
	bool loop = true, res = true;

	if (!path || !*path) {
		return false;
	}

	len = strlen(path);
	if ((path[len - 1] == '/') || (path[len - 1] == '\\')) {
		len--;
	}

	filter = (char *)z_malloc(len + 3);

	CopyMemory(filter, path, len);
	filter[len    ] = '\\';
	filter[len + 1] = '*';
	filter[len + 2] = '\0';

	h = FindFirstFileEx(filter, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
	if (h == INVALID_HANDLE_VALUE) {
		LOG_E("failed to open directory %s: %s", path, win32_error());
		res = false;
		goto free_filter;
	}

	do {
		if (fd.cFileName[0] == '.') {
			if (fd.cFileName[1] == '\0') {
				continue;
			} else {
				if ((fd.cFileName[1] == '.') && (fd.cFileName[2] == '\0')) {
					continue;
				}
			}
		}

		switch (cb(_ctx, fd.cFileName)) {
		case ir_cont:
			break;
		case ir_done:
			loop = false;
			break;
		case ir_err:
			loop = false;
			res = false;
			break;
		}
	} while (loop && FindNextFile(h, &fd));

	if (!FindClose(h)) {
		LOG_E("failed to close handle: %s", win32_error());
		res = false;
	}

free_filter:
	z_free(filter);

	return res;
}

bool
fs_make_symlink(const char *target, const char *path, bool force)
{
	return false;
	(void)target;
	(void)path;
	(void)force;
}

const char *
fs_user_home(void)
{
	return getenv("USERPROFILE");
}

static inline bool
is_wprefix(const WCHAR *s, const WCHAR *prefix)
{
	return (wcsncmp(s, prefix, sizeof(prefix) / sizeof(WCHAR) - 1) == 0);
}

bool
fs_is_a_tty_from_fd(int fd)
{
	HANDLE h;
	DWORD mode;

	h = (HANDLE *)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	/* first, test if the stream is associated to a Windows console */
	if (GetConsoleMode(h, &mode)) {
		return true;
	}
	/*
	 * if not, test if the stream is associated to a mintty-based terminal
	 * based on:
	 * https://fossies.org/linux/vim/src/iscygpty.c
	 * https://sourceforge.net/p/mingw-w64/mailman/message/35589741/
	 */
	else {
		FILE_NAME_INFO *fni;
		WCHAR *p = NULL;
		size_t size;
		size_t l;

		if (GetFileType(h) != FILE_TYPE_PIPE) {
			return false;
		}

		size = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * (MAX_PATH - 1);
		fni = z_malloc(size + sizeof(WCHAR));

		if (GetFileInformationByHandleEx(h, FileNameInfo, fni, size)) {
			fni->FileName[fni->FileNameLength / sizeof(WCHAR)] = L'\0';
			/*
			Check the name of the pipe:
			'\{cygwin,msys}-XXXXXXXXXXXXXXXX-ptyN-{from,to}-master'
			*/
			p = fni->FileName;
			if (is_wprefix(p, L"\\cygwin-")) {
				p += 8;
			} else if (is_wprefix(p, L"\\msys-")) {
				p += 6;
			} else {
				p = NULL;
			}
			if (p) {
				/* Skip 16-digit hexadecimal. */
				if (wcsspn(p, L"0123456789abcdefABCDEF") == 16) {
					p+= 16;

				} else {
					p = NULL;
				}
			}
			if (p) {
				if (is_wprefix(p, L"-pty")) {
					p += 4;
				}
				else {
					p = NULL;
				}
			}
			if (p) {
				/* Skip pty number. */
				l = wcsspn(p, L"0123456789");
				if (l >= 1 && l <= 4) {
					p += l;
				} else {
					p = NULL;

				}
				if (p) {
					if (is_wprefix(p, L"-from-master")) {
						//p += 12;
					} else if (is_wprefix(p, L"-to-master")) {
						 //p += 10;
					}
					else {
						p = NULL;
					}
				}
			}
		}

		z_free(fni);

		return p != NULL;
	}
}

bool
fs_chmod(const char *path, uint32_t mode)
{
	return false;
	(void)path;
	(void)mode;
}

bool
fs_copy_metadata(const char *src, const char *dest)
{
	return false;
	(void)src;
	(void)dest;
}

bool
fs_has_extension(const char *path, const char *ext)
{
	char *s;

	s = strrchr(path, '.');
	if (!s) {
		return false;
	}

	return lstrcmpi(s, ext) == 0;
}
