/*
 * Paint (C port) - what the program needs from the operating system
 *
 * os_posix.c (Linux, macOS) and os_win.c (Windows XP SP3 and later, built
 * with mingw-w64) implement it. Nothing outside os/ and host/ includes a
 * platform header. Strings are UTF-8 everywhere; os_win.c converts at the
 * edge. Every function may be called from any thread unless it says not.
 */
#ifndef OS_OS_H
#define OS_OS_H

#include <stddef.h>
#include <stdint.h>

/* ---- start-up ---- */
void os_init(void);                         /* once, first thing in main (sockets, the clock) */

/* ---- time ---- */
double os_now(void);                        /* seconds on a monotonic clock */
int64_t os_wall_ms(void);                   /* milliseconds since 1970 (UTC) */
void os_sleep_ms(int ms);
/* The local time of a wall-clock moment (seconds since 1970) */
typedef struct
{
    int year, month, day, hour, min, sec;   /* month 1..12 */
} os_tm;
void os_localtime(int64_t secs, os_tm *out);

/* ---- threads ---- */
typedef struct os_thread os_thread;
typedef struct os_mutex os_mutex;
typedef struct os_event os_event;          /* auto-reset: one wait returns per signal */

os_thread *os_thread_start(void (*fn)(void *arg), void *arg);
void os_thread_join(os_thread *t);          /* waits and frees */
void os_thread_detach(os_thread *t);        /* frees the handle; the thread runs on */

os_mutex *os_mutex_new(void);
void os_mutex_free(os_mutex *m);
void os_lock(os_mutex *m);
void os_unlock(os_mutex *m);

os_event *os_event_new(void);
void os_event_free(os_event *e);
void os_event_signal(os_event *e);
int os_event_wait(os_event *e, int timeout_ms);     /* 1 signalled, 0 timed out; timeout -1 = forever */

/* ---- sockets (IPv4) ----
 * A socket is an int (a SOCKET on Windows fits: the values are small).
 * Calls block unless os_sock_nonblock was used. -1 means failure. */
typedef int os_sock;
#define OS_BAD_SOCK (-1)

typedef struct
{
    uint32_t ip;        /* host order: 192.168.1.4 is 0xC0A80104 */
    uint16_t port;
} os_addr;

int os_addr_parse(const char *text, uint16_t default_port, os_addr *out);    /* "1.2.3.4" or "1.2.3.4:5"; 0 ok */
int os_resolve(const char *host, uint32_t *ip);                              /* a host name's IPv4 address (DNS); 0 ok */
void os_addr_format(os_addr a, char *out, size_t cap);                        /* "1.2.3.4:5" */

os_sock os_tcp_listen(uint16_t port, uint16_t *bound);      /* port 0 = any; *bound gets the real one */
os_sock os_tcp_accept(os_sock s, os_addr *peer);
os_sock os_tcp_connect(os_addr to, int timeout_ms);
os_sock os_udp_open(uint16_t port, int reuse);              /* bound to INADDR_ANY:port (0 = any) */
int os_udp_join(os_sock s, uint32_t group, uint32_t iface); /* multicast membership; iface 0 = default */
int os_udp_multicast_if(os_sock s, uint32_t iface);         /* where multicasts are sent from */
int os_udp_send(os_sock s, os_addr to, const void *data, int len);
int os_udp_recv(os_sock s, void *buf, int cap, os_addr *from, int timeout_ms);    /* bytes, 0 timeout, -1 error */
int os_send_all(os_sock s, const void *data, int len);     /* 0 ok, -1 failed */
int os_recv(os_sock s, void *buf, int cap);                 /* bytes, 0 closed, -1 error */
int os_recv_all(os_sock s, void *buf, int len);             /* 0 ok, -1 closed or failed */
int os_wait_readable(os_sock s, int timeout_ms);            /* 1 readable, 0 timed out, -1 error */
int os_sock_timeout(os_sock s, int recv_ms, int send_ms);   /* 0 = none */
void os_sock_nodelay(os_sock s);
void os_sock_shutdown(os_sock s);                           /* wakes a thread blocked in recv on it */
void os_sock_close(os_sock s);
os_addr os_sock_local(os_sock s);                           /* the address the socket is bound to */
/* This computer's IPv4 addresses (not loopback), best first; returns how many */
int os_local_ips(uint32_t *out, int cap);

/* ---- files and paths ---- */
char *os_read_file(const char *path, size_t *len);          /* malloc'd (and NUL-terminated), or NULL */
int os_write_file(const char *path, const void *data, size_t len);  /* 0 ok; written to a temp file, then renamed */
int os_file_size(const char *path, int64_t *size, int64_t *mtime_secs);   /* 0 ok */
int os_mkdirs(const char *path);                            /* 0 ok (also when it exists) */
/* The folder Paint keeps its settings in, created on first use (no trailing slash):
 * $XDG_CONFIG_HOME/paint or ~/.config/paint; %APPDATA%\Paint on Windows */
const char *os_config_dir(void);
const char *os_home_dir(void);
/* List a folder: calls fn(name, is_dir, user) for each entry but "." and ".." */
int os_list_dir(const char *dir, void (*fn)(const char *name, int is_dir, void *user), void *user);
int os_is_dir(const char *path);
char *os_path_join(const char *dir, const char *name);      /* malloc'd */
const char *os_path_base(const char *path);                 /* the part after the last / or \ */

/* ---- processes ---- */
/* Run a program and capture its standard output (malloc'd, NUL-terminated) and exit
 * status. argv[0] is looked up on PATH. Returns NULL if it could not be started. */
char *os_run_capture(char *const argv[], int *status);
int os_getpid(void);
/* Random bytes from the OS (falls back to a time-seeded generator) */
void os_random(void *out, size_t len);

#endif
