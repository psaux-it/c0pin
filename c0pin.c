/* SPDX-License-Identifier: MIT */
/*
 * c0pin - CPU C-state and frequency pinning tool
 *
 * Prevents CPU cores from entering deep idle (C-state) sleep and from
 * scaling down clock frequency, in order to eliminate latency jitter
 * for time-sensitive workloads (real-time capture, low-latency audio,
 * industrial control loops, etc.).
 *
 * Two operating modes:
 *
 *   --performance-policy
 *       Sets the cpufreq governor to "performance" and, where
 *       supported, the Energy Performance Preference (EPP) to
 *       "performance" on every cpufreq policy. Does not touch turbo,
 *       boost, or idle-state (C-state) behavior.
 *
 *   --aggressive [latency_us]
 *       Everything above, plus:
 *         - disables intel_pstate turbo throttling (no_turbo=0,
 *           max_perf_pct=100) when intel_pstate is loaded
 *         - enables global or per-policy CPU boost
 *         - raises scaling_max_freq to cpuinfo_max_freq, and
 *           additionally pins scaling_min_freq to the same value on
 *           every driver except amd-pstate, which keeps CPPC min-perf
 *           kernel-controlled by design and would misrepresent that
 *           as a fixed frequency if scaling_min_freq were forced to
 *           match
 *         - opens /dev/cpu_dma_latency and holds a PM QoS "CPU DMA
 *           latency" request for the process lifetime, which blocks
 *           the idle governor from selecting deep C-states. This is
 *           the primary mechanism behind the tool's name: it "pins"
 *           cores in C0.
 *       Runs as a foreground daemon in this mode, holding the QoS
 *       request open until SIGTERM/SIGINT, at which point the request
 *       is released and all other changes are left in place (they are
 *       not reverted on exit).
 *
 * Exit codes:
 *   0  success (including partial/unsupported knobs on hardware that
 *      doesn't expose them - see RC_UNSUPPORTED handling)
 *   1  a genuine I/O or verification failure occurred
 *
 * Must be run as root: all operations write to sysfs (cpufreq,
 * intel_pstate) and to /dev/cpu_dma_latency, both of which require
 * elevated privileges.
 *
 * Author:  Hasan CALISIR <hasan.calisir@psauxit.com>
 * License: MIT
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CPUFREQ_ROOT "/sys/devices/system/cpu/cpufreq"
#define INTEL_PSTATE_ROOT "/sys/devices/system/cpu/intel_pstate"
#define CPU_DMA_LATENCY_DEVICE "/dev/cpu_dma_latency"
#define SYSFS_BUF 4096U
#define PATH_BUF 512U

enum rc_class {
    RC_OK = 0,
    RC_UNSUPPORTED = 2,
    RC_ERROR = 1,
};

struct policy_state {
    char path[PATH_BUF];
    unsigned long cpuinfo_min;
    unsigned long cpuinfo_max;
    unsigned long scaling_min;
    unsigned long scaling_max;
    char driver[128];
    char governor[128];
    char epp[128];
    bool is_amd_pstate;
};

/*
 * Parses a non-negative 32-bit microsecond value. Rejects empty input,
 * trailing garbage, overflow, and negative numbers -- including -1
 * (PM_QOS_DEFAULT_VALUE / "no constraint"), which the kernel itself
 * would accept but which doesn't fit this tool's "bounded latency" use.
 */
static int parse_latency_us(const char *arg, int32_t *out)
{
    if (arg == NULL || *arg == '\0')
        return -1;

    errno = 0;
    char *endptr = NULL;
    long val = strtol(arg, &endptr, 10);

    if (endptr == arg)
        return -1; /* no digits were consumed at all */

    /* Reject trailing garbage, tolerating trailing whitespace only. */
    while (*endptr != '\0' && isspace((unsigned char)*endptr))
        endptr++;
    if (*endptr != '\0')
        return -1;

    if (errno == ERANGE)
        return -1;

    if (val < 0 || val > INT32_MAX)
        return -1;

    *out = (int32_t)val;
    return 0;
}

static void trim_ws(char *s)
{
    char *p;
    size_t n;

    if (!s)
        return;

    p = s;
    while (*p && isspace((unsigned char)*p))
        ++p;

    if (p != s)
        memmove(s, p, strlen(p) + 1U);

    n = strlen(s);
    while (n > 0U && isspace((unsigned char)s[n - 1U]))
        s[--n] = '\0';
}

static int build_path(char *dst, size_t dstsz, const char *base, const char *name)
{
    int n;

    if (!dst || !base || !name || dstsz == 0U) {
        errno = EINVAL;
        return -1;
    }

    n = snprintf(dst, dstsz, "%s/%s", base, name);
    if (n < 0 || (size_t)n >= dstsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int read_sysfs(const char *path, char *buf, size_t bufsz)
{
    int fd;
    ssize_t n;
    int saved_errno;

    if (!path || !buf || bufsz < 2U) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    for (;;) {
        n = read(fd, buf, bufsz - 1U);
        if (n >= 0)
            break;
        if (errno != EINTR) {
            saved_errno = errno;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
    }

    if (n == (ssize_t)(bufsz - 1U)) {
        saved_errno = EOVERFLOW;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    if (close(fd) < 0)
        return -1;

    buf[n] = '\0';
    trim_ws(buf);
    return 0;
}

static int write_sysfs(const char *path, const char *value)
{
    int fd;
    size_t len;
    ssize_t n;
    int saved_errno;

    if (!path || !value) {
        errno = EINVAL;
        return -1;
    }

    len = strlen(value);
    if (len == 0U) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    for (;;) {
        n = write(fd, value, len);
        if (n >= 0)
            break;
        if (errno != EINTR) {
            saved_errno = errno;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
    }

    if ((size_t)n != len) {
        saved_errno = EIO;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    if (close(fd) < 0)
        return -1;

    return 0;
}

static int read_ulong_sysfs(const char *path, unsigned long *value)
{
    char buf[128];
    char *end = NULL;
    unsigned long v;

    if (!value) {
        errno = EINVAL;
        return -1;
    }

    if (read_sysfs(path, buf, sizeof(buf)) < 0)
        return -1;

    if (buf[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    v = strtoul(buf, &end, 10);
    if (errno == ERANGE || end == buf || *end != '\0') {
        errno = EINVAL;
        return -1;
    }

    *value = v;
    return 0;
}

static int write_ulong_sysfs(const char *path, unsigned long value)
{
    char buf[64];
    int n;

    n = snprintf(buf, sizeof(buf), "%lu\n", value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        errno = EOVERFLOW;
        return -1;
    }

    return write_sysfs(path, buf);
}

static bool list_contains(const char *list, const char *needle)
{
    const char *p;
    size_t needle_len;

    if (!list || !needle || needle[0] == '\0')
        return false;

    needle_len = strlen(needle);
    p = list;

    while (*p) {
        const char *start;
        const char *end;

        while (*p && isspace((unsigned char)*p))
            ++p;
        if (!*p)
            break;

        start = p;
        while (*p && !isspace((unsigned char)*p))
            ++p;
        end = p;

        if ((size_t)(end - start) == needle_len &&
            memcmp(start, needle, needle_len) == 0)
            return true;
    }

    return false;
}

static bool is_amd_pstate_driver(const char *driver)
{
    return driver && (!strcmp(driver, "amd-pstate") ||
                       !strcmp(driver, "amd-pstate-epp"));
}

static int read_policy_field(const char *policy, const char *name,
                             char *buf, size_t bufsz)
{
    char path[PATH_BUF];

    if (build_path(path, sizeof(path), policy, name) < 0)
        return -1;
    return read_sysfs(path, buf, bufsz);
}

static int read_policy_ulong(const char *policy, const char *name,
                             unsigned long *value)
{
    char path[PATH_BUF];

    if (build_path(path, sizeof(path), policy, name) < 0)
        return -1;
    return read_ulong_sysfs(path, value);
}

static int write_policy_ulong(const char *policy, const char *name,
                              unsigned long value)
{
    char path[PATH_BUF];

    if (build_path(path, sizeof(path), policy, name) < 0)
        return -1;
    return write_ulong_sysfs(path, value);
}

static int write_policy_string(const char *policy, const char *name,
                               const char *value)
{
    char path[PATH_BUF];

    if (build_path(path, sizeof(path), policy, name) < 0)
        return -1;
    return write_sysfs(path, value);
}

static int policy_number(const char *path, unsigned long *value)
{
    const char *base;
    const char *p;
    char *end = NULL;
    unsigned long v;

    if (!path || !value) {
        errno = EINVAL;
        return -1;
    }

    base = strrchr(path, '/');
    base = base ? base + 1 : path;

    if (strncmp(base, "policy", 6) != 0 || base[6] == '\0') {
        errno = EINVAL;
        return -1;
    }

    for (p = base + 6; *p; ++p) {
        if (*p < '0' || *p > '9') {
            errno = EINVAL;
            return -1;
        }
    }

    errno = 0;
    v = strtoul(base + 6, &end, 10);
    if (errno == ERANGE || end == base + 6 || *end != '\0') {
        errno = EINVAL;
        return -1;
    }

    *value = v;
    return 0;
}

static int read_policy_state(const char *policy, struct policy_state *st)
{
    memset(st, 0, sizeof(*st));
    if (snprintf(st->path, sizeof(st->path), "%s", policy) >=
        (int)sizeof(st->path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (read_policy_ulong(policy, "cpuinfo_min_freq", &st->cpuinfo_min) < 0)
        return -1;
    if (read_policy_ulong(policy, "cpuinfo_max_freq", &st->cpuinfo_max) < 0)
        return -1;
    if (read_policy_ulong(policy, "scaling_min_freq", &st->scaling_min) < 0)
        return -1;
    if (read_policy_ulong(policy, "scaling_max_freq", &st->scaling_max) < 0)
        return -1;
    if (read_policy_field(policy, "scaling_driver", st->driver,
                          sizeof(st->driver)) < 0)
        return -1;
    if (read_policy_field(policy, "scaling_governor", st->governor,
                          sizeof(st->governor)) < 0)
        return -1;

    st->epp[0] = '\0';
    {
        char epp_path[PATH_BUF];
        if (build_path(epp_path, sizeof(epp_path), policy,
                       "energy_performance_preference") == 0) {
            if (read_sysfs(epp_path, st->epp, sizeof(st->epp)) < 0 &&
                errno != ENOENT && errno != EOPNOTSUPP) {
                return -1;
            }
        }
    }

    st->is_amd_pstate = is_amd_pstate_driver(st->driver);
    return 0;
}

static int governor_is_performance(const char *policy)
{
    char governor[128];

    if (read_policy_field(policy, "scaling_governor", governor,
                          sizeof(governor)) < 0)
        return -1;

    return strcmp(governor, "performance") == 0 ? 1 : 0;
}

static int set_governor_performance(const char *policy)
{
    char governors[SYSFS_BUF];
    int state;

    if (read_policy_field(policy, "scaling_available_governors",
                          governors, sizeof(governors)) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (!list_contains(governors, "performance"))
        return RC_UNSUPPORTED;

    state = governor_is_performance(policy);
    if (state < 0)
        return RC_ERROR;
    if (state == 1)
        return RC_OK;

    if (write_policy_string(policy, "scaling_governor", "performance\n") < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    state = governor_is_performance(policy);
    if (state < 0)
        return RC_ERROR;
    return state == 1 ? RC_OK : RC_ERROR;
}

static bool epp_is_performance(const char *value)
{
    char *end = NULL;
    unsigned long v;

    if (!value || value[0] == '\0')
        return false;

    if (!strcmp(value, "performance"))
        return true;

    errno = 0;
    v = strtoul(value, &end, 10);
    return errno == 0 && end != value && *end == '\0' && v == 0UL;
}

static int set_epp_performance(const char *policy)
{
    char current[128];
    char available[SYSFS_BUF];
    char path[PATH_BUF];
    int err;

    if (build_path(path, sizeof(path), policy,
                   "energy_performance_preference") < 0)
        return RC_ERROR;

    if (read_sysfs(path, current, sizeof(current)) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (epp_is_performance(current))
        return RC_OK;

    available[0] = '\0';
    if (read_policy_field(policy, "energy_performance_available_preferences",
                          available, sizeof(available)) < 0 &&
        errno != ENOENT && errno != EOPNOTSUPP) {
        return RC_ERROR;
    }

    if (available[0] != '\0' && list_contains(available, "performance"))
        err = write_sysfs(path, "performance\n");
    else
        err = write_sysfs(path, "0\n");

    if (err < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL ||
            errno == EBUSY)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_sysfs(path, current, sizeof(current)) < 0)
        return RC_ERROR;

    return epp_is_performance(current) ? RC_OK : RC_ERROR;
}

static bool sysfs_attr_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode));
}

static int intel_pstate_mode(char *mode, size_t modesz)
{
    char path[PATH_BUF];
    char value[64];
    struct stat st;

    if (mode && modesz)
        mode[0] = '\0';

    if (stat(INTEL_PSTATE_ROOT, &st) < 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    if (!S_ISDIR(st.st_mode))
        return 0;

    if (build_path(path, sizeof(path), INTEL_PSTATE_ROOT, "status") < 0)
        return -1;

    if (read_sysfs(path, value, sizeof(value)) == 0) {
        if (!strcmp(value, "active") || !strcmp(value, "passive")) {
            size_t len = strlen(value);
            if (mode && modesz) {
                if (len + 1U > modesz) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                memcpy(mode, value, len + 1U);
            }
            return 1;
        }

        if (!strcmp(value, "off"))
            return 0;

        errno = EPROTO;
        return -1;
    }

    if (errno != ENOENT && errno != EOPNOTSUPP)
        return -1;

    {
        char no_turbo[PATH_BUF];
        char max_perf_pct[PATH_BUF];

        if (build_path(no_turbo, sizeof(no_turbo), INTEL_PSTATE_ROOT,
                       "no_turbo") < 0)
            return -1;
        if (build_path(max_perf_pct, sizeof(max_perf_pct), INTEL_PSTATE_ROOT,
                       "max_perf_pct") < 0)
            return -1;

        if (sysfs_attr_exists(no_turbo) || sysfs_attr_exists(max_perf_pct)) {
            static const char legacy[] = "legacy";
            if (mode && modesz) {
                if (sizeof(legacy) > modesz) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                memcpy(mode, legacy, sizeof(legacy));
            }
            return 1;
        }
    }

    return 0;
}

static int set_intel_pstate_no_turbo_zero(void)
{
    char path[PATH_BUF];
    unsigned long value;

    if (build_path(path, sizeof(path), INTEL_PSTATE_ROOT, "no_turbo") < 0)
        return RC_ERROR;

    if (read_ulong_sysfs(path, &value) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }
    if (value > 1UL)
        return RC_ERROR;

    if (value != 0UL && write_ulong_sysfs(path, 0UL) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL ||
            errno == EBUSY)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_ulong_sysfs(path, &value) < 0)
        return RC_ERROR;

    return value == 0UL ? RC_OK : RC_ERROR;
}

static int set_intel_pstate_max_perf_100(void)
{
    char path[PATH_BUF];
    unsigned long value;

    if (build_path(path, sizeof(path), INTEL_PSTATE_ROOT,
                   "max_perf_pct") < 0)
        return RC_ERROR;

    if (read_ulong_sysfs(path, &value) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }
    if (value > 100UL)
        return RC_ERROR;

    if (value != 100UL && write_ulong_sysfs(path, 100UL) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL ||
            errno == EBUSY)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_ulong_sysfs(path, &value) < 0)
        return RC_ERROR;

    return value == 100UL ? RC_OK : RC_ERROR;
}

static int set_global_boost_enabled(bool intel_pstate_loaded)
{
    char path[PATH_BUF];
    unsigned long value;

    if (intel_pstate_loaded)
        return RC_UNSUPPORTED;

    if (build_path(path, sizeof(path), CPUFREQ_ROOT, "boost") < 0)
        return RC_ERROR;

    if (read_ulong_sysfs(path, &value) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }
    if (value > 1UL)
        return RC_ERROR;

    if (value == 0UL && write_ulong_sysfs(path, 1UL) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL ||
            errno == EBUSY)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_ulong_sysfs(path, &value) < 0)
        return RC_ERROR;

    return value == 1UL ? RC_OK : RC_ERROR;
}

static int set_policy_boost_enabled(const char *policy)
{
    char path[PATH_BUF];
    unsigned long value;

    /*
     * Per-policy boost is exposed as policy<N>/boost -- not
     * "local_boost", despite the kernel's internal symbol names.
     * Linux 6.6+, and only present if the driver supports it.
     */
    if (build_path(path, sizeof(path), policy, "boost") < 0)
        return RC_ERROR;

    if (read_ulong_sysfs(path, &value) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }
    if (value > 1UL)
        return RC_ERROR;

    if (value == 0UL && write_ulong_sysfs(path, 1UL) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL ||
            errno == EBUSY)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_ulong_sysfs(path, &value) < 0)
        return RC_ERROR;

    return value == 1UL ? RC_OK : RC_ERROR;
}

static int set_policy_max_effective(const char *policy,
                                    unsigned long target,
                                    unsigned long *effective)
{
    unsigned long v;

    if (write_policy_ulong(policy, "scaling_max_freq", target) < 0) {
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_policy_ulong(policy, "scaling_max_freq", &v) < 0)
        return RC_ERROR;

    if (effective)
        *effective = v;

    return v == target ? RC_OK : RC_UNSUPPORTED;
}

static int aggressive_frequency_limit(struct policy_state *before,
                                      bool pin_min,
                                      unsigned long *effective_max)
{
    unsigned long max_after = 0UL;
    unsigned long min_after = 0UL;
    int rc;

    if (before->cpuinfo_max < before->cpuinfo_min)
        return RC_ERROR;

    rc = set_policy_max_effective(before->path, before->cpuinfo_max,
                                  &max_after);
    if (effective_max)
        *effective_max = max_after;

    if (rc == RC_ERROR)
        return RC_ERROR;

    if (rc == RC_UNSUPPORTED || max_after != before->cpuinfo_max)
        return RC_UNSUPPORTED;

    if (!pin_min)
        return RC_OK;

    if (write_policy_ulong(before->path, "scaling_min_freq", max_after) < 0) {
        int saved_errno = errno;
        /* max was already raised; restore both to their original values. */
        (void)write_policy_ulong(before->path, "scaling_min_freq",
                                 before->scaling_min);
        (void)write_policy_ulong(before->path, "scaling_max_freq",
                                 before->scaling_max);
        errno = saved_errno;
        if (errno == ENOENT || errno == EOPNOTSUPP || errno == EINVAL)
            return RC_UNSUPPORTED;
        return RC_ERROR;
    }

    if (read_policy_ulong(before->path, "scaling_min_freq", &min_after) < 0) {
        int saved_errno = errno;
        (void)write_policy_ulong(before->path, "scaling_min_freq",
                                 before->scaling_min);
        (void)write_policy_ulong(before->path, "scaling_max_freq",
                                 before->scaling_max);
        errno = saved_errno;
        return RC_ERROR;
    }
    if (read_policy_ulong(before->path, "scaling_max_freq", &max_after) < 0) {
        int saved_errno = errno;
        (void)write_policy_ulong(before->path, "scaling_min_freq",
                                 before->scaling_min);
        (void)write_policy_ulong(before->path, "scaling_max_freq",
                                 before->scaling_max);
        errno = saved_errno;
        return RC_ERROR;
    }

    if (effective_max)
        *effective_max = max_after;

    if (min_after == max_after && max_after == before->cpuinfo_max)
        return RC_OK;

    {
        (void)write_policy_ulong(before->path, "scaling_min_freq",
                                 before->scaling_min);
        (void)write_policy_ulong(before->path, "scaling_max_freq",
                                 before->scaling_max);
        errno = EIO;
    }
    return RC_UNSUPPORTED;
}

/*
 * Opens /dev/cpu_dma_latency, writes the latency bound, and returns the
 * held fd (the request stays active as long as it's open).
 *
 * O_RDWR: we read the fd back after writing to get the effective
 * system-wide aggregate (the MIN across all open requests, not just
 * ours) -- a write-only fd can't do that. If the readback fails,
 * *effective_us is left at -1; the write already succeeded, so we
 * still return a valid fd.
 */
static int acquire_cpu_dma_latency(int32_t latency_us, int32_t *effective_us)
{
    int fd;
    ssize_t n;

    if (effective_us)
        *effective_us = -1;

    fd = open(CPU_DMA_LATENCY_DEVICE, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;

    for (;;) {
        n = write(fd, &latency_us, sizeof(latency_us));
        if (n >= 0)
            break;
        if (errno != EINTR) {
            int saved_errno = errno;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }
    }

    if (n != (ssize_t)sizeof(latency_us)) {
        int saved_errno = EIO;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    if (lseek(fd, 0, SEEK_SET) == 0) {
        int32_t value = -1;
        ssize_t rn;

        for (;;) {
            rn = read(fd, &value, sizeof(value));
            if (rn >= 0)
                break;
            if (errno != EINTR)
                break;
        }

        if (rn == (ssize_t)sizeof(value) && effective_us)
            *effective_us = value;
    }

    return fd;
}

static int wait_for_shutdown(void)
{
    sigset_t set;
    int signo;

    if (sigemptyset(&set) != 0)
        return -1;
    if (sigaddset(&set, SIGINT) != 0 || sigaddset(&set, SIGTERM) != 0)
        return -1;
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        return -1;

    for (;;) {
        if (sigwait(&set, &signo) != 0) {
            errno = EIO;
            return -1;
        }
        if (signo == SIGINT || signo == SIGTERM)
            return 0;
    }
}

static int process_policy(const char *policy, bool aggressive,
                          bool global_boost_enabled, bool boost_fallback_required)
{
    struct policy_state before;
    unsigned long effective_max = 0UL;
    int rc;
    int overall = RC_OK;

    if (read_policy_state(policy, &before) < 0) {
        fprintf(stderr, "ERROR: cannot read %s: %s\n", policy,
                strerror(errno));
        return RC_ERROR;
    }

    printf("%s %s\n", aggressive ? "[aggressive]" : "[performance-policy]",
           policy);
    printf("  driver=%s governor=%s cpuinfo=[%lu,%lu] policy=[%lu,%lu]\n",
           before.driver, before.governor, before.cpuinfo_min,
           before.cpuinfo_max, before.scaling_min, before.scaling_max);

    rc = set_governor_performance(policy);
    if (rc == RC_ERROR) {
        fprintf(stderr, "ERROR: %s: cannot set performance governor: %s\n",
                policy, strerror(errno));
        return RC_ERROR;
    }
    if (rc == RC_UNSUPPORTED) {
        fprintf(stderr, "WARN: %s: performance governor unsupported\n", policy);
        overall = RC_UNSUPPORTED;
    }

    rc = set_epp_performance(policy);
    if (rc == RC_ERROR) {
        fprintf(stderr, "ERROR: %s: EPP configuration failed: %s\n",
                policy, strerror(errno));
        return RC_ERROR;
    }
    if (rc == RC_UNSUPPORTED) {
        fprintf(stderr, "WARN: %s: EPP performance unsupported\n", policy);
        if (overall == RC_OK)
            overall = RC_UNSUPPORTED;
    }

    if (aggressive) {
        if (!global_boost_enabled) {
            rc = set_policy_boost_enabled(policy);
            if (rc == RC_ERROR) {
                fprintf(stderr, "ERROR: %s: policy boost enable failed: %s\n",
                        policy, strerror(errno));
                return RC_ERROR;
            }
            if (rc == RC_UNSUPPORTED && boost_fallback_required) {
                fprintf(stderr, "WARN: %s: boost interface unavailable\n", policy);
                if (overall == RC_OK)
                    overall = RC_UNSUPPORTED;
            }
        }

        /*
         * amd-pstate keeps CPPC min-perf kernel-controlled under the
         * performance governor -- scaling_min_freq == max here doesn't
         * mean MinPerf is actually pinned to max.
         */
        rc = aggressive_frequency_limit(&before, !before.is_amd_pstate,
                                        &effective_max);
        if (rc == RC_ERROR) {
            fprintf(stderr,
                    "ERROR: %s: aggressive frequency policy failed: %s\n",
                    policy, strerror(errno));
            return RC_ERROR;
        }
        if (rc == RC_UNSUPPORTED) {
            fprintf(stderr,
                    "WARN: %s: requested frequency ceiling was clamped or rejected\n",
                    policy);
            if (overall == RC_OK)
                overall = RC_UNSUPPORTED;
        }
    }

    {
        struct policy_state after;
        if (read_policy_state(policy, &after) < 0) {
            fprintf(stderr, "ERROR: %s: final verification failed: %s\n",
                    policy, strerror(errno));
            return RC_ERROR;
        }

        printf("  final: driver=%s governor=%s", after.driver, after.governor);
        if (after.epp[0] != '\0')
            printf(" epp=%s", after.epp);
        printf(" policy=[%lu,%lu] cpuinfo=[%lu,%lu]",
               after.scaling_min, after.scaling_max,
               after.cpuinfo_min, after.cpuinfo_max);
        if (aggressive && after.is_amd_pstate)
            printf(" amd_min=kernel-controlled");
        printf("\n");

        if (strcmp(after.governor, "performance") != 0)
            overall = RC_ERROR;

        if (after.epp[0] != '\0' && !epp_is_performance(after.epp) &&
            overall == RC_OK)
            overall = RC_UNSUPPORTED;

        if (aggressive && effective_max != 0UL &&
            after.scaling_max != effective_max && overall == RC_OK)
            overall = RC_UNSUPPORTED;

        if (aggressive && !after.is_amd_pstate &&
            after.scaling_min != after.scaling_max && overall == RC_OK)
            overall = RC_UNSUPPORTED;
    }

    return overall;
}

/*
 * RC_UNSUPPORTED (knob doesn't exist on this CPU/kernel) is not a
 * failure -- only RC_ERROR exits non-zero. Keeps the systemd unit from
 * flapping "failed" on hardware that's just missing some of these knobs.
 */
static int process_exit_code(int overall)
{
    return overall == RC_ERROR ? 1 : 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--performance-policy | --aggressive [latency_us]]\n"
            "\n"
            "  --performance-policy   Set performance governor and EPP=performance.\n"
            "  --aggressive [latency_us]\n"
            "                          Above plus boost/turbo controls, maximum policy,\n"
            "                          and a persistent CPU DMA latency QoS request\n"
            "                          (non-negative microseconds; defaults to 0 if\n"
            "                          omitted). If this request cannot be acquired,\n"
            "                          the tool aborts before changing any CPU state.\n",
            prog);
}

int main(int argc, char **argv)
{
    bool aggressive;
    bool intel_loaded;
    char intel_mode[32];
    glob_t g = {0};
    size_t i;
    int overall = RC_OK;
    int rc;
    bool global_boost_enabled = false;
    bool boost_fallback_required = false;
    int dma_latency_fd = -1;
    int32_t latency_us = 0;
    sigset_t shutdown_set;

    if (geteuid() != 0) {
        fprintf(stderr, "ERROR: root privileges are required.\n");
        return RC_ERROR;
    }

    if (argc < 2 || argc > 3) {
        usage(argv[0]);
        return RC_ERROR;
    }

    if (!strcmp(argv[1], "--performance-policy")) {
        aggressive = false;
        if (argc != 2) {
            usage(argv[0]);
            return RC_ERROR;
        }
    } else if (!strcmp(argv[1], "--aggressive")) {
        aggressive = true;
        if (argc == 3) {
            if (parse_latency_us(argv[2], &latency_us) != 0) {
                fprintf(stderr, "Invalid latency value: %s\n", argv[2]);
                fprintf(stderr,
                        "Expected a non-negative integer number of microseconds.\n");
                return RC_ERROR;
            }
        }
    } else {
        usage(argv[0]);
        return RC_ERROR;
    }

    /* Block shutdown signals before changing any CPU state. */
    if (sigemptyset(&shutdown_set) != 0 ||
        sigaddset(&shutdown_set, SIGINT) != 0 ||
        sigaddset(&shutdown_set, SIGTERM) != 0 ||
        sigprocmask(SIG_BLOCK, &shutdown_set, NULL) != 0) {
        fprintf(stderr, "ERROR: cannot configure shutdown signal handling: %s\n",
                strerror(errno));
        return RC_ERROR;
    }

    rc = intel_pstate_mode(intel_mode, sizeof(intel_mode));
    if (rc < 0) {
        fprintf(stderr, "ERROR: cannot inspect intel_pstate: %s\n",
                strerror(errno));
        return RC_ERROR;
    }
    intel_loaded = rc == 1;

    printf("CPUFreq tuner\n");
    if (intel_loaded)
        printf("Intel P-State: %s\n", intel_mode);
    else
        printf("Intel P-State: not active\n");

    if (aggressive) {
        int32_t effective_us = -1;

        /*
         * Acquire the latency lock before touching any CPU state --
         * if this fails, abort now instead of running "aggressive"
         * mode without the C-state guarantee it's supposed to give.
         */
        dma_latency_fd = acquire_cpu_dma_latency(latency_us, &effective_us);
        if (dma_latency_fd < 0) {
            fprintf(stderr, "ERROR: cannot acquire %s=%d: %s\n",
                    CPU_DMA_LATENCY_DEVICE, latency_us, strerror(errno));
            fprintf(stderr,
                    "Aggressive mode requires a held CPU DMA latency QoS "
                    "request; aborting before any CPU state is changed.\n");
            return RC_ERROR;
        }

        if (effective_us >= 0) {
            printf("CPU DMA latency QoS: requested %d us, effective "
                   "system-wide aggregate %d us (fd=%d, held until exit)\n",
                   latency_us, effective_us, dma_latency_fd);
        } else {
            printf("CPU DMA latency QoS: requested %d us (fd=%d, held "
                   "until exit)\n", latency_us, dma_latency_fd);
        }

        if (intel_loaded) {
            rc = set_intel_pstate_no_turbo_zero();
            if (rc == RC_ERROR) {
                fprintf(stderr, "ERROR: intel_pstate no_turbo=0 failed: %s\n",
                        strerror(errno));
                overall = RC_ERROR;
            } else if (rc == RC_UNSUPPORTED) {
                fprintf(stderr,
                        "WARN: intel_pstate no_turbo control unavailable\n");
                if (overall == RC_OK)
                    overall = RC_UNSUPPORTED;
            }

            rc = set_intel_pstate_max_perf_100();
            if (rc == RC_ERROR) {
                fprintf(stderr,
                        "ERROR: intel_pstate max_perf_pct=100 failed: %s\n",
                        strerror(errno));
                overall = RC_ERROR;
            } else if (rc == RC_UNSUPPORTED) {
                fprintf(stderr,
                        "WARN: intel_pstate max_perf_pct control unavailable\n");
                if (overall == RC_OK)
                    overall = RC_UNSUPPORTED;
            }
        }

        rc = set_global_boost_enabled(intel_loaded);
        if (rc == RC_ERROR) {
            fprintf(stderr, "ERROR: generic boost enable failed: %s\n",
                    strerror(errno));
            overall = RC_ERROR;
        } else if (rc == RC_OK) {
            global_boost_enabled = true;
        } else if (rc == RC_UNSUPPORTED && !intel_loaded) {
            boost_fallback_required = true;
            fprintf(stderr,
                    "INFO: generic CPUFreq boost unavailable; checking policy-local boost\n");
        }
    }

    rc = glob(CPUFREQ_ROOT "/policy*", 0, NULL, &g);
    if (rc == GLOB_NOMATCH) {
        fprintf(stderr, "ERROR: no CPUFreq policies found under %s\n",
                CPUFREQ_ROOT);
        globfree(&g);
        if (dma_latency_fd >= 0)
            (void)close(dma_latency_fd);
        return RC_ERROR;
    }
    if (rc != 0) {
        fprintf(stderr, "ERROR: glob() failed: %d\n", rc);
        globfree(&g);
        if (dma_latency_fd >= 0)
            (void)close(dma_latency_fd);
        return RC_ERROR;
    }

    for (i = 0; i < g.gl_pathc; ++i) {
        struct stat st;
        unsigned long n;

        if (stat(g.gl_pathv[i], &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        if (policy_number(g.gl_pathv[i], &n) < 0)
            continue;

        rc = process_policy(g.gl_pathv[i], aggressive,
                            global_boost_enabled, boost_fallback_required);
        if (rc == RC_ERROR)
            overall = RC_ERROR;
        else if (rc == RC_UNSUPPORTED && overall == RC_OK)
            overall = RC_UNSUPPORTED;
    }

    globfree(&g);

    if (overall == RC_ERROR) {
        if (dma_latency_fd >= 0)
            (void)close(dma_latency_fd);
        fprintf(stderr, "Result: ERROR\n");
        return RC_ERROR;
    }

    if (!aggressive) {
        printf("Result: performance-policy mode complete\n");
        printf("Status: %s\n", overall == RC_OK ? "OK" : "PARTIAL/UNSUPPORTED");
        return process_exit_code(overall);
    }

    printf("Result: aggressive configuration complete\n");
    printf("Status: %s\n", overall == RC_OK ? "OK" : "PARTIAL/UNSUPPORTED");

    /* dma_latency_fd is guaranteed valid here: we returned RC_ERROR above
     * if acquiring it failed, before any CPU state was touched. */
    printf("Aggressive lock active. Send SIGTERM or SIGINT to release CPU DMA latency QoS and exit.\n");
    fflush(stdout);

    rc = wait_for_shutdown();
    if (rc < 0) {
        fprintf(stderr, "ERROR: shutdown wait failed: %s\n", strerror(errno));
        (void)close(dma_latency_fd);
        return RC_ERROR;
    }

    printf("Shutdown signal received. Releasing CPU DMA latency QoS.\n");
    (void)close(dma_latency_fd);
    return process_exit_code(overall);
}
