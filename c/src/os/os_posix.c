/*
 * Paint (C port) - the OS layer on Linux and other POSIX systems
 */
#include "os/os.h"
#include <stdatomic.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void os_init(void)
{
    signal(SIGPIPE, SIG_IGN);
}

/* ---- time ---- */
double os_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int64_t os_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void os_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

void os_localtime(int64_t secs, os_tm *out)
{
    time_t t = (time_t)secs;
    struct tm tm;
    localtime_r(&t, &tm);
    out->year = tm.tm_year + 1900;
    out->month = tm.tm_mon + 1;
    out->day = tm.tm_mday;
    out->hour = tm.tm_hour;
    out->min = tm.tm_min;
    out->sec = tm.tm_sec;
}

/* ---- threads ---- */
struct os_thread
{
    pthread_t t;
    void (*fn)(void *);
    void *arg;
    atomic_int refs;    /* the thread, and whoever started it (until join or detach) */
};

struct os_mutex
{
    pthread_mutex_t m;
};

struct os_event
{
    pthread_mutex_t m;
    pthread_cond_t c;
    int set;
};

static void thread_unref(os_thread *t)
{
    if (atomic_fetch_sub(&t->refs, 1) == 1)
        free(t);
}

static void *thread_main(void *p)
{
    os_thread *t = p;
    void (*fn)(void *) = t->fn;
    void *arg = t->arg;
    thread_unref(t);
    fn(arg);
    return NULL;
}

os_thread *os_thread_start(void (*fn)(void *arg), void *arg)
{
    os_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    atomic_init(&t->refs, 2);
    if (pthread_create(&t->t, NULL, thread_main, t) != 0)
    {
        free(t);
        return NULL;
    }
    return t;
}

void os_thread_join(os_thread *t)
{
    if (!t)
        return;
    pthread_join(t->t, NULL);
    thread_unref(t);
}

void os_thread_detach(os_thread *t)
{
    if (!t)
        return;
    pthread_detach(t->t);
    thread_unref(t);
}

os_mutex *os_mutex_new(void)
{
    os_mutex *m = calloc(1, sizeof *m);
    pthread_mutex_init(&m->m, NULL);
    return m;
}

void os_mutex_free(os_mutex *m)
{
    if (!m)
        return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void os_lock(os_mutex *m)
{
    pthread_mutex_lock(&m->m);
}

void os_unlock(os_mutex *m)
{
    pthread_mutex_unlock(&m->m);
}

os_event *os_event_new(void)
{
    os_event *e = calloc(1, sizeof *e);
    pthread_mutex_init(&e->m, NULL);
    pthread_cond_init(&e->c, NULL);
    return e;
}

void os_event_free(os_event *e)
{
    if (!e)
        return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}

void os_event_signal(os_event *e)
{
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}

int os_event_wait(os_event *e, int timeout_ms)
{
    pthread_mutex_lock(&e->m);
    if (timeout_ms < 0)
    {
        while (!e->set)
            pthread_cond_wait(&e->c, &e->m);
    }
    else
    {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += timeout_ms / 1000;
        until.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000)
        {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        while (!e->set)
            if (pthread_cond_timedwait(&e->c, &e->m, &until) == ETIMEDOUT)
                break;
    }
    int got = e->set;
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

/* ---- sockets ---- */
static struct sockaddr_in to_sin(os_addr a)
{
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(a.ip);
    sin.sin_port = htons(a.port);
    return sin;
}

static os_addr from_sin(const struct sockaddr_in *sin)
{
    os_addr a = { ntohl(sin->sin_addr.s_addr), ntohs(sin->sin_port) };
    return a;
}

int os_addr_parse(const char *text, uint16_t default_port, os_addr *out)
{
    unsigned a, b, c, d, port = default_port;
    char tail;
    int n = sscanf(text, " %u.%u.%u.%u:%u %c", &a, &b, &c, &d, &port, &tail);
    if (n < 4 || n == 6 || a > 255 || b > 255 || c > 255 || d > 255 || port > 65535)
        return -1;
    if (n == 4 && sscanf(text, " %u.%u.%u.%u %c", &a, &b, &c, &d, &tail) == 5)
        return -1;
    out->ip = a << 24 | b << 16 | c << 8 | d;
    out->port = (uint16_t)port;
    return 0;
}

int os_resolve(const char *host, uint32_t *ip)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res)
        return -1;
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return 0;
}

void os_addr_format(os_addr a, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u:%u", a.ip >> 24, a.ip >> 16 & 255, a.ip >> 8 & 255, a.ip & 255, a.port);
}

static int cloexec(int fd)
{
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

os_sock os_tcp_listen(uint16_t port, uint16_t *bound)
{
    int s = cloexec(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0)
        return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sin = to_sin((os_addr){ 0, port });
    if (bind(s, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(s, 16) != 0)
    {
        close(s);
        return -1;
    }
    if (bound)
        *bound = os_sock_local(s).port;
    return s;
}

os_sock os_tcp_accept(os_sock s, os_addr *peer)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    int c;
    do
        c = accept(s, (struct sockaddr *)&sin, &len);
    while (c < 0 && errno == EINTR);
    if (c < 0)
        return -1;
    cloexec(c);
    if (peer)
        *peer = from_sin(&sin);
    return c;
}

os_sock os_tcp_connect(os_addr to, int timeout_ms)
{
    int s = cloexec(socket(AF_INET, SOCK_STREAM, 0));
    if (s < 0)
        return -1;
    int flags = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in sin = to_sin(to);
    int r = connect(s, (struct sockaddr *)&sin, sizeof sin);
    if (r != 0 && errno == EINPROGRESS)
    {
        struct pollfd p = { s, POLLOUT, 0 };
        int err = 0;
        socklen_t len = sizeof err;
        if (poll(&p, 1, timeout_ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
            r = 0;
    }
    if (r != 0)
    {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, flags);
    return s;
}

os_sock os_udp_open(uint16_t port, int reuse)
{
    int s = cloexec(socket(AF_INET, SOCK_DGRAM, 0));
    if (s < 0)
        return -1;
    if (reuse)
    {
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    }
    struct sockaddr_in sin = to_sin((os_addr){ 0, port });
    if (bind(s, (struct sockaddr *)&sin, sizeof sin) != 0)
    {
        close(s);
        return -1;
    }
    return s;
}

int os_udp_join(os_sock s, uint32_t group, uint32_t iface)
{
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = htonl(group);
    m.imr_interface.s_addr = htonl(iface);
    return setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) == 0 ? 0 : -1;
}

int os_udp_multicast_if(os_sock s, uint32_t iface)
{
    struct in_addr a;
    a.s_addr = htonl(iface);
    unsigned char ttl = 255;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    return setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a) == 0 ? 0 : -1;
}

int os_udp_send(os_sock s, os_addr to, const void *data, int len)
{
    struct sockaddr_in sin = to_sin(to);
    return (int)sendto(s, data, (size_t)len, 0, (struct sockaddr *)&sin, sizeof sin);
}

int os_udp_recv(os_sock s, void *buf, int cap, os_addr *from, int timeout_ms)
{
    int w = os_wait_readable(s, timeout_ms);
    if (w <= 0)
        return w;
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    ssize_t n = recvfrom(s, buf, (size_t)cap, 0, (struct sockaddr *)&sin, &len);
    if (n < 0)
        return errno == EINTR || errno == EAGAIN ? 0 : -1;
    if (from)
        *from = from_sin(&sin);
    return (int)n;
}

int os_send_all(os_sock s, const void *data, int len)
{
    const char *p = data;
    while (len > 0)
    {
        ssize_t n = send(s, p, (size_t)len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (int)n;
    }
    return 0;
}

int os_recv(os_sock s, void *buf, int cap)
{
    ssize_t n;
    do
        n = recv(s, buf, (size_t)cap, 0);
    while (n < 0 && errno == EINTR);
    return (int)n;
}

int os_recv_all(os_sock s, void *buf, int len)
{
    char *p = buf;
    while (len > 0)
    {
        int n = os_recv(s, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int os_wait_readable(os_sock s, int timeout_ms)
{
    struct pollfd p = { s, POLLIN, 0 };
    int r;
    do
        r = poll(&p, 1, timeout_ms);
    while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : r > 0;
}

int os_sock_timeout(os_sock s, int recv_ms, int send_ms)
{
    struct timeval r = { recv_ms / 1000, (recv_ms % 1000) * 1000 }, w = { send_ms / 1000, (send_ms % 1000) * 1000 };
    int a = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r);
    int b = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof w);
    return a == 0 && b == 0 ? 0 : -1;
}

void os_sock_nodelay(os_sock s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void os_sock_shutdown(os_sock s)
{
    shutdown(s, SHUT_RDWR);
}

void os_sock_close(os_sock s)
{
    if (s >= 0)
        close(s);
}

os_addr os_sock_local(os_sock s)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    memset(&sin, 0, sizeof sin);
    getsockname(s, (struct sockaddr *)&sin, &len);
    return from_sin(&sin);
}

/* Private (RFC 1918) addresses first: that is where friends are */
static int ip_rank(uint32_t ip)
{
    if ((ip >> 24) == 192 && (ip >> 16 & 255) == 168)
        return 0;
    if ((ip >> 24) == 10 || ((ip >> 24) == 172 && (ip >> 16 & 255) >= 16 && (ip >> 16 & 255) < 32))
        return 1;
    if ((ip >> 24) == 169 && (ip >> 16 & 255) == 254)
        return 3;
    return 2;
}

int os_local_ips(uint32_t *out, int cap)
{
    struct ifaddrs *list, *i;
    int n = 0;
    if (getifaddrs(&list) != 0)
        return 0;
    for (i = list; i && n < cap; i = i->ifa_next)
    {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
            continue;
        uint32_t ip = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        int dup = 0;
        for (int k = 0; k < n; k++)
            dup |= out[k] == ip;
        if (!dup)
            out[n++] = ip;
    }
    freeifaddrs(list);
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && ip_rank(out[b]) < ip_rank(out[b - 1]); b--)
        {
            uint32_t t = out[b];
            out[b] = out[b - 1];
            out[b - 1] = t;
        }
    return n;
}

/* ---- files ---- */
char *os_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    for (;;)
    {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap)
            break;
        cap *= 2;
        buf = realloc(buf, cap + 1);
    }
    int bad = ferror(f);
    fclose(f);
    if (bad)
    {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len)
        *len = n;
    return buf;
}

void os_trim(void)
{
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

const void *os_map_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    void *p = MAP_FAILED;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= SIZE_MAX)
        p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fd >= 0)
        close(fd);
    if (p == MAP_FAILED)
        return NULL;
    *len = (size_t)st.st_size;
    return p;
}

void os_unmap_file(const void *data, size_t len)
{
    if (data)
        munmap((void *)data, len);
}

int os_write_file(const char *path, const void *data, size_t len)
{
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 16);
    snprintf(tmp, plen + 16, "%s.tmp%d", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f)
        ok &= fclose(f) == 0;
    ok = ok && rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

int os_file_size(const char *path, int64_t *size, int64_t *mtime_secs)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    if (size)
        *size = st.st_size;
    if (mtime_secs)
        *mtime_secs = st.st_mtime;
    return 0;
}

int os_mkdirs(const char *path)
{
    char *p = strdup(path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/')
        {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    int r = mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
    free(p);
    return r;
}

const char *os_home_dir(void)
{
    static char home[1024];
    if (!home[0])
    {
        const char *h = getenv("HOME");
        if (!h || !*h)
        {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(home, sizeof home, "%s", h);
    }
    return home;
}

const char *os_config_dir(void)
{
    static char dir[1100];
    if (!dir[0])
    {
        const char *x = getenv("PAINT_CONFIG_DIR");          /* the tests keep their own settings */
        if (x && *x)
            snprintf(dir, sizeof dir, "%s", x);
        else if ((x = getenv("XDG_CONFIG_HOME")) && *x == '/')
            snprintf(dir, sizeof dir, "%s/paint", x);
        else
            snprintf(dir, sizeof dir, "%s/.config/paint", os_home_dir());
        os_mkdirs(dir);
    }
    return dir;
}

int os_list_dir(const char *dir, void (*fn)(const char *name, int is_dir, void *user), void *user)
{
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        int is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK)
        {
            char *full = os_path_join(dir, e->d_name);
            is_dir = os_is_dir(full);
            free(full);
        }
        fn(e->d_name, is_dir, user);
    }
    closedir(d);
    return 0;
}

int os_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *os_path_join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *out = malloc(a + b + 2);
    memcpy(out, dir, a);
    size_t n = a;
    if (a && dir[a - 1] != '/')
        out[n++] = '/';
    memcpy(out + n, name, b + 1);
    return out;
}

const char *os_path_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ---- processes ---- */
char *os_run_capture(char *const argv[], int *status)
{
    int fds[2];
    if (pipe(fds) != 0)
        return NULL;
    int alive[2];
    if (pipe(alive) != 0)
    {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    cloexec(alive[1]);
    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        close(alive[0]);
        close(alive[1]);
        return NULL;
    }
    if (pid == 0)
    {
        close(alive[0]);
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0)
        {
            dup2(null, 0);
            dup2(null, 2);
        }
        execvp(argv[0], argv);
        char failed = 1;
        if (write(alive[1], &failed, 1) < 0)
            _exit(127);
        _exit(127);
    }
    close(fds[1]);
    close(alive[1]);
    char failed = 0;
    int started = read(alive[0], &failed, 1) != 1;     /* closed by exec: it started */
    close(alive[0]);
    size_t cap = 4096, n = 0;
    char *out = malloc(cap + 1);
    for (;;)
    {
        ssize_t got = read(fds[0], out + n, cap - n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        n += (size_t)got;
        if (n == cap)
        {
            cap *= 2;
            out = realloc(out, cap + 1);
        }
    }
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!started)
    {
        free(out);
        return NULL;
    }
    out[n] = 0;
    if (status)
        *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return out;
}

int os_getpid(void)
{
    return (int)getpid();
}

void os_random(void *out, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t n = 0;
    if (fd >= 0)
    {
        while (n < len)
        {
            ssize_t got = read(fd, (char *)out + n, len - n);
            if (got <= 0)
                break;
            n += (size_t)got;
        }
        close(fd);
    }
    if (n < len)
    {
        uint64_t x = (uint64_t)os_wall_ms() ^ (uint64_t)getpid() << 32 ^ (uint64_t)(os_now() * 1e9);
        for (; n < len; n++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            ((unsigned char *)out)[n] = (unsigned char)x;
        }
    }
}
