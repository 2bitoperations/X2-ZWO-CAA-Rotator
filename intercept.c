/*
 * intercept.c — LD_PRELOAD shim to capture ZWO CAA HID feature report traffic
 *
 * Intercepts ioctl() calls on /dev/hidraw* and logs the full buffer contents
 * for HIDIOCSFEATURE (host→device) and HIDIOCGFEATURE (device→host).
 *
 * Build:
 *   gcc -O2 -fPIC -shared -o intercept.so intercept.c -ldl
 *
 * Use:
 *   CAA_INTERCEPT_LOG=caa_traffic.log LD_PRELOAD=./intercept.so ./test_caa
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── real function pointers ─────────────────────────────────────────────── */
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static int (*real_open)(const char *, int, ...) = NULL;
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_openat)(int, const char *, int, ...) = NULL;
static int (*real_close)(int) = NULL;

/* ── fd → device name table ─────────────────────────────────────────────── */
#define MAX_FDS 1024
static char fd_path[MAX_FDS][64];

static void track_fd(int fd, const char *path)
{
    if (fd >= 0 && fd < MAX_FDS)
        snprintf(fd_path[fd], sizeof(fd_path[0]), "%s", path);
}

static void untrack_fd(int fd)
{
    if (fd >= 0 && fd < MAX_FDS)
        fd_path[fd][0] = '\0';
}

static int is_hidraw_fd(int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return 0;
    return strncmp(fd_path[fd], "/dev/hidraw", 11) == 0;
}

/* ── log file ───────────────────────────────────────────────────────────── */
static FILE *log_fp = NULL;
static int   log_seq = 0;

static void log_init(void)
{
    if (log_fp) return;
    const char *path = getenv("CAA_INTERCEPT_LOG");
    if (!path) path = "caa_intercept.log";
    log_fp = fopen(path, "a");
    if (!log_fp) log_fp = stderr;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(log_fp,
            "\n# ============================================================\n"
            "# intercept started  pid=%d  t=%.6f\n"
            "# ============================================================\n",
            getpid(), ts.tv_sec + ts.tv_nsec * 1e-9);
    fflush(log_fp);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void log_bytes(const char *dir, int fd, const unsigned char *buf, int len)
{
    log_init();
    fprintf(log_fp, "[%.6f] seq=%-4d  %s  fd=%d  dev=%s  len=%d\n",
            now_sec(), ++log_seq, dir, fd,
            (fd >= 0 && fd < MAX_FDS) ? fd_path[fd] : "?", len);

    /* hex dump */
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0)
            fprintf(log_fp, "  %04x  ", i);
        fprintf(log_fp, "%02x ", buf[i]);
        if (i % 16 == 15 || i == len - 1) {
            /* pad */
            for (int p = (i % 16) + 1; p < 16; p++)
                fprintf(log_fp, "   ");
            fprintf(log_fp, " |");
            int row_start = i - (i % 16);
            for (int j = row_start; j <= i; j++)
                fprintf(log_fp, "%c", (buf[j] >= 0x20 && buf[j] < 0x7f) ? buf[j] : '.');
            fprintf(log_fp, "|\n");
        }
    }
    fflush(log_fp);
}

/* ── real function resolution ───────────────────────────────────────────── */
static void resolve(void)
{
    if (!real_ioctl) {
        real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
        real_open   = dlsym(RTLD_NEXT, "open");
        real_open64 = dlsym(RTLD_NEXT, "open64");
        real_openat = dlsym(RTLD_NEXT, "openat");
        real_close  = dlsym(RTLD_NEXT, "close");
    }
}

/* ── intercepted open ───────────────────────────────────────────────────── */
int open(const char *path, int flags, ...)
{
    resolve();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = real_open(path, flags, mode);
    if (fd >= 0 && path && strncmp(path, "/dev/hidraw", 11) == 0)
        track_fd(fd, path);
    return fd;
}

int open64(const char *path, int flags, ...)
{
    resolve();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = real_open64(path, flags, mode);
    if (fd >= 0 && path && strncmp(path, "/dev/hidraw", 11) == 0)
        track_fd(fd, path);
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...)
{
    resolve();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    int fd = real_openat(dirfd, path, flags, mode);
    if (fd >= 0 && path && strncmp(path, "/dev/hidraw", 11) == 0)
        track_fd(fd, path);
    return fd;
}

/* ── intercepted close ──────────────────────────────────────────────────── */
int close(int fd)
{
    resolve();
    if (is_hidraw_fd(fd)) {
        log_init();
        fprintf(log_fp, "[%.6f] seq=%-4d  CLOSE  fd=%d  dev=%s\n",
                now_sec(), ++log_seq, fd, fd_path[fd]);
        fflush(log_fp);
        untrack_fd(fd);
    }
    return real_close(fd);
}

/* ── intercepted ioctl ──────────────────────────────────────────────────── */
int ioctl(int fd, unsigned long request, ...)
{
    resolve();
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (!is_hidraw_fd(fd))
        return real_ioctl(fd, request, arg);

    /* Extract HID ioctl type and size from the request number */
    unsigned int ioc_type = _IOC_TYPE(request);
    unsigned int ioc_nr   = _IOC_NR(request);
    unsigned int ioc_size = _IOC_SIZE(request);

    int is_set = (ioc_type == 'H' && ioc_nr == 0x06);  /* HIDIOCSFEATURE */
    int is_get = (ioc_type == 'H' && ioc_nr == 0x07);  /* HIDIOCGFEATURE */

    /* For SET: log buffer BEFORE the call */
    if (is_set && arg)
        log_bytes("HIDIOCSFEATURE >>", fd, (const unsigned char *)arg, ioc_size);

    int ret = real_ioctl(fd, request, arg);

    /* For GET: log buffer AFTER the call (device filled it in) */
    if (is_get && arg && ret > 0)
        log_bytes("HIDIOCGFEATURE <<", fd, (const unsigned char *)arg, ret);
    else if (is_get && arg && ret == 0)
        log_bytes("HIDIOCGFEATURE <<", fd, (const unsigned char *)arg, ioc_size);

    /* Log any other hidraw ioctls briefly */
    if (!is_set && !is_get) {
        log_init();
        fprintf(log_fp, "[%.6f] seq=%-4d  IOCTL  fd=%d  request=0x%08lx  ret=%d\n",
                now_sec(), ++log_seq, fd, request, ret);
        fflush(log_fp);
    }

    return ret;
}
