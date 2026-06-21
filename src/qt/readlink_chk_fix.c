/* SEQUENTIA: in-binary fix for a glibc FORTIFY false-abort in bundled Qt 5.9.
 *
 * Qt's QLockFile::processNameByPid() -> qt_readlink() (reached when QSettings
 * syncs its conf file, e.g. during OptionsModel migration at GUI startup) calls
 * readlink() with a length larger than the compiler-known destination size, so
 * glibc's __readlink_chk() aborts with "*** buffer overflow detected ***".
 *
 * Defining __readlink_chk / __readlinkat_chk here (a direct object in the
 * elements-qt link) interposes glibc's versions for the whole binary. We clamp
 * the requested length to the real buffer size before calling the underlying
 * syscall: this can never overflow the buffer and avoids the spurious abort.
 *
 * Linux/glibc only. On Windows (mingw) there is no readlink/readlinkat and no
 * glibc FORTIFY, so the guard makes this an empty translation unit there.
 * Guard on __linux__ (a compiler-predefined macro): __GLIBC__ would only be
 * defined after a libc header is included, which is too late for the #ifdef.
 */
#ifdef __linux__
#include <sys/types.h> /* ssize_t, size_t */

extern ssize_t readlink(const char *path, char *buf, size_t bufsiz);
extern ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);

ssize_t __readlink_chk(const char *path, char *buf, size_t len, size_t buflen)
{
    if (len > buflen) len = buflen;
    return readlink(path, buf, len);
}

ssize_t __readlinkat_chk(int dirfd, const char *path, char *buf, size_t len, size_t buflen)
{
    if (len > buflen) len = buflen;
    return readlinkat(dirfd, path, buf, len);
}
#endif /* __linux__ */
