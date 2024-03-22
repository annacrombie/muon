/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-FileCopyrightText: Vincent Torri <vtorri@outlook.fr>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "compat.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "log.h"
#include "lang/string.h"
#include "platform/mem.h"
#include "platform/filesystem.h"
#include "platform/path.h"
#include "platform/windows/log.h"
#include "platform/windows/win32_error.h"

bool tty_is_pty = true;

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
	HANDLE h;
	HANDLE fm;
	unsigned char *base;
	unsigned char *iter;
	unsigned __int64 size;
	DWORD size_high;
	DWORD size_low;
	DWORD offset;
	bool ret = false;

	h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return ret;
	}

	size_low = GetFileSize(h, &size_high);
	if ((size_low == INVALID_FILE_SIZE) && (GetLastError() != NO_ERROR)) {
		LOG_I("can not get the size of file %s", path);
		goto close_file;
	}

	size = size_low | ((__int64)size_high << 32);

	/*
	 * PE file is organized as followed:
	 *  1) MS-DOS header (60 bytes) (beginning with bytes 'M' then 'Z')
	 *  2) offset of PE signature (4 bytes)"
	 *  2) PE signature (4 bytes) : "PE\0\0"
	 *  3) COFF File Header (20 bytes)
	 * the rest is useless for us.
	 */
	if (size < 64) {
		LOG_I("file %s is too small", path);
		goto close_file;
	}

	fm = CreateFileMapping(h, NULL, PAGE_READONLY, 0UL, 0UL, NULL);
	if (!fm) {
		LOG_I("Can not map file: %s", win32_error());
		goto close_file;
	}

	base =  MapViewOfFile(fm, FILE_MAP_READ, 0, 0, 0);
	if (!base) {
		LOG_I("Can not view map: %s", win32_error());
		goto close_fm;
	}

	iter = base;

	if (*((WORD *)iter) != 0x5a4d) {
		LOG_I("file %s is not a MS-DOS file", path);
		goto unmap;
	}

	offset = *((DWORD *)(iter + 0x3c));
	if (size < offset + 24) {
		LOG_I("file %s is too small", path);
		goto unmap;
	}

	iter += offset;
	if ((iter[0] != 'P') && (iter[1] != 'E') &&
	    (iter[2] != '\0') && (iter[3] != '\0')) {
		LOG_I("file %s is not a PE file", path);
		goto unmap;
	}

	iter += 22;
	if ((!((*((WORD *)iter)) & 0x0002)) || ((*((WORD *)iter)) & 0x2000)) {
		LOG_I("file %s is not a binary file", path);
		goto unmap;
	}

	ret = true;

unmap:
	UnmapViewOfFile(base);
close_fm:
	CloseHandle(fm);
close_file:
	CloseHandle(h);

	return ret;
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
fs_rmdir(const char *path)
{
	if (!RemoveDirectory(path)) {
		LOG_E("failed to remove directory %s: %s\n", path, win32_error());
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
	return wcsncmp(s, prefix, sizeof(prefix) / sizeof(WCHAR) - 1) == 0;
}

bool
fs_is_a_tty_from_fd(int fd)
{
	HANDLE h;
	DWORD mode;
	bool ret = false;

	h = (HANDLE *)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		return ret;
	}

	/* first, check if the handle is assocoated with a conpty-based terminal */
	{
		HMODULE mod;

		mod = LoadLibrary("kernel32.dll");
		if (mod) {
			if (GetProcAddress(mod, "ClosePseudoConsole")) {
				if (GetConsoleMode(h, &mode)) {
					// ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT
					mode |= 0x4 | 0x1;
					if (SetConsoleMode(h, mode)) {
						FreeLibrary(mod);
						tty_is_pty = true;
						return true;
					}
				}
			}
			FreeLibrary(mod);
		}
	}

	/*
	 * test if the stream is associated to a mintty-based terminal
	 * based on:
	 * https://fossies.org/linux/vim/src/iscygpty.c
	 * https://sourceforge.net/p/mingw-w64/mailman/message/35589741/
	 * it means mintty without conpty
	 */
	{
		if (GetFileType(h) == FILE_TYPE_PIPE) {
			FILE_NAME_INFO *fni;
			WCHAR *p = NULL;
			size_t size;
			size_t l;

			size = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * (MAX_PATH - 1);
			fni = z_malloc(size + sizeof(WCHAR));

			if (GetFileInformationByHandleEx(h, FileNameInfo, fni, size)) {
				fni->FileName[fni->FileNameLength / sizeof(WCHAR)] = L'\0';
				/*
				 * Check the name of the pipe:
				 * '\{cygwin,msys}-XXXXXXXXXXXXXXXX-ptyN-{from,to}-master'
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
						p += 16;
					} else {
						p = NULL;
					}
				}
				if (p) {
					if (is_wprefix(p, L"-pty")) {
						p += 4;
					}else {
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
						}else {
							p = NULL;
						}
					}
				}
			}
			z_free(fni);
			if (p) {
				tty_is_pty = true;
				return true;
			}
		}
	}

	/*
	 * last case: cmd without conpty
	 */
	if (GetConsoleMode(h, &mode)) {
		tty_is_pty = false;
		return true;
	}

	return false;
}

bool
fs_chmod(const char *path, uint32_t mode)
{
	int mask = _S_IREAD;

	if (mode & _S_IWRITE) {
		mask |= _S_IWRITE;
	}

	if (_chmod(path, mask) == -1) {
		LOG_E("failed chmod(%s, %o): %s", path, mode, strerror(errno));
		return false;
	}

	return true;
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

bool
fs_find_cmd(struct workspace *wk, struct sbuf *buf, const char *cmd)
{
	assert(*cmd);
	uint32_t len;
	const char *env_path, *base_start;

	sbuf_clear(buf);

	if (!path_is_basename(cmd)) {
		path_make_absolute(wk, buf, cmd);

		if (fs_exe_exists(buf->buf)) {
			return true;
		} else {
			if (!fs_has_extension(buf->buf, ".exe")) {
				sbuf_pushs(wk, buf, ".exe");
				if (fs_exe_exists(buf->buf)) {
					return true;
				}
			}
			return false;
		}
	}

	if (!(env_path = getenv("PATH"))) {
		LOG_E("failed to get the value of PATH");
		return false;
	}

	base_start = env_path;
	while (true) {
		if (!*env_path || *env_path == ';') {
			len = env_path - base_start;

			sbuf_clear(buf);
			sbuf_pushn(wk, buf, base_start, len);

			base_start = env_path + 1;

			path_push(wk, buf, cmd);

			if (fs_exe_exists(buf->buf)) {
				return true;
			}else if (!fs_has_extension(buf->buf, ".exe")) {
				sbuf_pushs(wk, buf, ".exe");
				if (fs_exe_exists(buf->buf)) {
					return true;
				}
			}

			if (!*env_path) {
				break;
			}
		}

		++env_path;
	}

	return false;
}

enum fs_mtime_result
fs_mtime(const char *path, int64_t *mtime)
{
	WIN32_FILE_ATTRIBUTE_DATA d;
	ULARGE_INTEGER t;
	if (!GetFileAttributesEx(path, GetFileExInfoStandard, &d)) {
		return fs_mtime_result_not_found;
	}

	t.LowPart = d.ftLastWriteTime.dwLowDateTime;
	t.HighPart = d.ftLastWriteTime.dwHighDateTime;
	*mtime = t.QuadPart * 100;
	return fs_mtime_result_ok;
}

bool
fs_remove(const char *path)
{
	if (!DeleteFileA(path)) {
		LOG_E("failed DeleteFile(\"%s\"): %s", path, win32_error());
		return false;
	}

	return true;
}
