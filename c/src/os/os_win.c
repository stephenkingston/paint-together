/*
 * Paint (C port) - the OS layer on Windows (XP SP3 and later, mingw-w64)
 *
 * Paths and process arguments are UTF-8 at the API and UTF-16 underneath.
 */
#include "os/os.h"
#include <stdatomic.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wchar_t *wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        w[0] = 0;
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL))
        s[0] = 0;
    return s;
}

static LARGE_INTEGER g_freq;

void os_init(void)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    QueryPerformanceFrequency(&g_freq);
}

/* ---- time ---- */
double os_now(void)
{
    LARGE_INTEGER t;
    if (!g_freq.QuadPart)
        QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_freq.QuadPart;
}

int64_t os_wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000);
}

void os_sleep_ms(int ms)
{
    Sleep((DWORD)ms);
}

void os_localtime(int64_t secs, os_tm *out)
{
    uint64_t t = (uint64_t)secs * 10000000ULL + 116444736000000000ULL;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) }, local;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    out->year = st.wYear;
    out->month = st.wMonth;
    out->day = st.wDay;
    out->hour = st.wHour;
    out->min = st.wMinute;
    out->sec = st.wSecond;
}

/* ---- threads ---- */
struct os_thread
{
    HANDLE h;
    void (*fn)(void *);
    void *arg;
    atomic_int refs;    /* the thread, and whoever started it (until join or detach) */
};

struct os_mutex
{
    CRITICAL_SECTION cs;
};

struct os_event
{
    HANDLE h;
};

static void thread_unref(os_thread *t)
{
    if (atomic_fetch_sub(&t->refs, 1) == 1)
        free(t);
}

static DWORD WINAPI thread_main(LPVOID p)
{
    os_thread *t = p;
    void (*fn)(void *) = t->fn;
    void *arg = t->arg;
    thread_unref(t);
    fn(arg);
    return 0;
}

os_thread *os_thread_start(void (*fn)(void *arg), void *arg)
{
    os_thread *t = calloc(1, sizeof *t);
    t->fn = fn;
    t->arg = arg;
    atomic_init(&t->refs, 2);
    t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
    if (!t->h)
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
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    thread_unref(t);
}

void os_thread_detach(os_thread *t)
{
    if (!t)
        return;
    CloseHandle(t->h);
    thread_unref(t);
}

os_mutex *os_mutex_new(void)
{
    os_mutex *m = calloc(1, sizeof *m);
    InitializeCriticalSection(&m->cs);
    return m;
}

void os_mutex_free(os_mutex *m)
{
    if (!m)
        return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

void os_lock(os_mutex *m)
{
    EnterCriticalSection(&m->cs);
}

void os_unlock(os_mutex *m)
{
    LeaveCriticalSection(&m->cs);
}

os_event *os_event_new(void)
{
    os_event *e = calloc(1, sizeof *e);
    e->h = CreateEventW(NULL, FALSE, FALSE, NULL);
    return e;
}

void os_event_free(os_event *e)
{
    if (!e)
        return;
    CloseHandle(e->h);
    free(e);
}

void os_event_signal(os_event *e)
{
    SetEvent(e->h);
}

int os_event_wait(os_event *e, int timeout_ms)
{
    return WaitForSingleObject(e->h, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) == WAIT_OBJECT_0;
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

static SOCKET S(os_sock s)
{
    return (SOCKET)(intptr_t)s;
}

static os_sock wrap(SOCKET s)
{
    return s == INVALID_SOCKET ? OS_BAD_SOCK : (os_sock)(intptr_t)s;
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

os_sock os_tcp_listen(uint16_t port, uint16_t *bound)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return OS_BAD_SOCK;
    BOOL excl = TRUE;   /* Windows' SO_REUSEADDR would let two hosts share a port */
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    struct sockaddr_in sin = to_sin((os_addr){ 0, port });
    if (bind(s, (struct sockaddr *)&sin, sizeof sin) != 0 || listen(s, 16) != 0)
    {
        closesocket(s);
        return OS_BAD_SOCK;
    }
    if (bound)
        *bound = os_sock_local(wrap(s)).port;
    return wrap(s);
}

os_sock os_tcp_accept(os_sock s, os_addr *peer)
{
    struct sockaddr_in sin;
    int len = sizeof sin;
    SOCKET c = accept(S(s), (struct sockaddr *)&sin, &len);
    if (c == INVALID_SOCKET)
        return OS_BAD_SOCK;
    if (peer)
        *peer = from_sin(&sin);
    return wrap(c);
}

os_sock os_tcp_connect(os_addr to, int timeout_ms)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET)
        return OS_BAD_SOCK;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    struct sockaddr_in sin = to_sin(to);
    int r = connect(s, (struct sockaddr *)&sin, sizeof sin);
    if (r != 0 && WSAGetLastError() == WSAEWOULDBLOCK)
    {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(0, NULL, &w, &e, &tv) == 1 && FD_ISSET(s, &w))
            r = 0;
    }
    if (r != 0)
    {
        closesocket(s);
        return OS_BAD_SOCK;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    return wrap(s);
}

os_sock os_udp_open(uint16_t port, int reuse)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return OS_BAD_SOCK;
    if (reuse)
    {
        BOOL one = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    }
    struct sockaddr_in sin = to_sin((os_addr){ 0, port });
    if (bind(s, (struct sockaddr *)&sin, sizeof sin) != 0)
    {
        closesocket(s);
        return OS_BAD_SOCK;
    }
    return wrap(s);
}

int os_udp_join(os_sock s, uint32_t group, uint32_t iface)
{
    struct ip_mreq m;
    m.imr_multiaddr.s_addr = htonl(group);
    m.imr_interface.s_addr = htonl(iface);
    return setsockopt(S(s), IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&m, sizeof m) == 0 ? 0 : -1;
}

int os_udp_multicast_if(os_sock s, uint32_t iface)
{
    struct in_addr a;
    a.s_addr = htonl(iface);
    DWORD ttl = 255;
    setsockopt(S(s), IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof ttl);
    return setsockopt(S(s), IPPROTO_IP, IP_MULTICAST_IF, (const char *)&a, sizeof a) == 0 ? 0 : -1;
}

int os_udp_send(os_sock s, os_addr to, const void *data, int len)
{
    struct sockaddr_in sin = to_sin(to);
    return sendto(S(s), data, len, 0, (struct sockaddr *)&sin, sizeof sin);
}

int os_udp_recv(os_sock s, void *buf, int cap, os_addr *from, int timeout_ms)
{
    int w = os_wait_readable(s, timeout_ms);
    if (w <= 0)
        return w;
    struct sockaddr_in sin;
    int len = sizeof sin;
    int n = recvfrom(S(s), buf, cap, 0, (struct sockaddr *)&sin, &len);
    if (n < 0)
    {
        int e = WSAGetLastError();
        return e == WSAECONNRESET || e == WSAEMSGSIZE || e == WSAEWOULDBLOCK ? 0 : -1;
    }
    if (from)
        *from = from_sin(&sin);
    return n;
}

int os_send_all(os_sock s, const void *data, int len)
{
    const char *p = data;
    while (len > 0)
    {
        int n = send(S(s), p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int os_recv(os_sock s, void *buf, int cap)
{
    return recv(S(s), buf, cap, 0);
}

int os_recv_all(os_sock s, void *buf, int len)
{
    char *p = buf;
    while (len > 0)
    {
        int n = recv(S(s), p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int os_wait_readable(os_sock s, int timeout_ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(S(s), &r);
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    int n = select(0, &r, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    return n < 0 ? -1 : n > 0;
}

int os_sock_timeout(os_sock s, int recv_ms, int send_ms)
{
    DWORD r = (DWORD)recv_ms, w = (DWORD)send_ms;
    int a = setsockopt(S(s), SOL_SOCKET, SO_RCVTIMEO, (const char *)&r, sizeof r);
    int b = setsockopt(S(s), SOL_SOCKET, SO_SNDTIMEO, (const char *)&w, sizeof w);
    return a == 0 && b == 0 ? 0 : -1;
}

void os_sock_nodelay(os_sock s)
{
    BOOL one = TRUE;
    setsockopt(S(s), IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
}

void os_sock_shutdown(os_sock s)
{
    shutdown(S(s), SD_BOTH);
}

void os_sock_close(os_sock s)
{
    if (s != OS_BAD_SOCK)
        closesocket(S(s));
}

os_addr os_sock_local(os_sock s)
{
    struct sockaddr_in sin;
    int len = sizeof sin;
    memset(&sin, 0, sizeof sin);
    getsockname(S(s), (struct sockaddr *)&sin, &len);
    return from_sin(&sin);
}

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
    ULONG size = 0;
    int n = 0;
    GetAdaptersInfo(NULL, &size);
    IP_ADAPTER_INFO *list = size ? malloc(size) : NULL;
    if (list && GetAdaptersInfo(list, &size) == NO_ERROR)
        for (IP_ADAPTER_INFO *a = list; a && n < cap; a = a->Next)
            for (IP_ADDR_STRING *s = &a->IpAddressList; s && n < cap; s = s->Next)
            {
                uint32_t ip = ntohl(inet_addr(s->IpAddress.String));
                if (ip == 0 || ip == 0xFFFFFFFFu || (ip >> 24) == 127)
                    continue;
                int dup = 0;
                for (int k = 0; k < n; k++)
                    dup |= out[k] == ip;
                if (!dup)
                    out[n++] = ip;
            }
    free(list);
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
    wchar_t *w = wide(path);
    FILE *f = _wfopen(w, L"rb");
    free(w);
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

int os_write_file(const char *path, const void *data, size_t len)
{
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 16);
    snprintf(tmp, plen + 16, "%s.tmp%d", path, (int)GetCurrentProcessId());
    wchar_t *wt = wide(tmp), *wp = wide(path);
    FILE *f = _wfopen(wt, L"wb");
    int ok = f && fwrite(data, 1, len, f) == len;
    if (f)
        ok &= fclose(f) == 0;
    ok = ok && MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING);
    if (!ok)
        DeleteFileW(wt);
    free(wt);
    free(wp);
    free(tmp);
    return ok ? 0 : -1;
}

int os_file_size(const char *path, int64_t *size, int64_t *mtime_secs)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = wide(path);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok)
        return -1;
    if (size)
        *size = (int64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    if (mtime_secs)
    {
        uint64_t t = (uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime;
        *mtime_secs = (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
    }
    return 0;
}

int os_mkdirs(const char *path)
{
    char *p = strdup(path);
    for (char *s = p + 1; *s; s++)
        if ((*s == '/' || *s == '\\') && s[-1] != ':')
        {
            char c = *s;
            *s = 0;
            wchar_t *w = wide(p);
            CreateDirectoryW(w, NULL);
            free(w);
            *s = c;
        }
    wchar_t *w = wide(p);
    int r = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
    free(w);
    free(p);
    return r;
}

const char *os_home_dir(void)
{
    static char home[1024];
    if (!home[0])
    {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, buf)))
        {
            char *s = narrow(buf);
            snprintf(home, sizeof home, "%s", s);
            free(s);
        }
        else
            snprintf(home, sizeof home, "C:\\");
    }
    return home;
}

const char *os_config_dir(void)
{
    static char dir[1100];
    if (!dir[0])
    {
        wchar_t buf[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, buf)))
        {
            char *s = narrow(buf);
            snprintf(dir, sizeof dir, "%s\\Paint", s);
            free(s);
        }
        else
            snprintf(dir, sizeof dir, "%s\\Paint", os_home_dir());
        os_mkdirs(dir);
    }
    return dir;
}

int os_list_dir(const char *dir, void (*fn)(const char *name, int is_dir, void *user), void *user)
{
    char *pattern = os_path_join(dir, "*");
    wchar_t *w = wide(pattern);
    free(pattern);
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(w, &d);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do
    {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L".."))
            continue;
        char *name = narrow(d.cFileName);
        fn(name, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
        free(name);
    }
    while (FindNextFileW(h, &d));
    FindClose(h);
    return 0;
}

int os_is_dir(const char *path)
{
    wchar_t *w = wide(path);
    DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

char *os_path_join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *out = malloc(a + b + 2);
    memcpy(out, dir, a);
    size_t n = a;
    if (a && dir[a - 1] != '/' && dir[a - 1] != '\\')
        out[n++] = '\\';
    memcpy(out + n, name, b + 1);
    return out;
}

const char *os_path_base(const char *path)
{
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

/* ---- processes ---- */
char *os_run_capture(char *const argv[], int *status)
{
    /* Quote each argument the way CommandLineToArgvW reads it */
    size_t cap = 256, n = 0;
    char *cmd = malloc(cap);
    for (int i = 0; argv[i]; i++)
    {
        size_t need = n + strlen(argv[i]) * 2 + 4;
        if (need > cap)
        {
            cap = need * 2;
            cmd = realloc(cmd, cap);
        }
        if (i)
            cmd[n++] = ' ';
        cmd[n++] = '"';
        for (const char *s = argv[i]; *s; s++)
        {
            if (*s == '"')
                cmd[n++] = '\\';
            cmd[n++] = *s;
        }
        cmd[n++] = '"';
    }
    cmd[n] = 0;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
    {
        free(cmd);
        return NULL;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t *wcmd = wide(cmd);
    free(cmd);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(wcmd);
    CloseHandle(wr);
    if (!ok)
    {
        CloseHandle(rd);
        return NULL;
    }
    size_t ocap = 4096, on = 0;
    char *out = malloc(ocap + 1);
    DWORD got;
    while (ReadFile(rd, out + on, (DWORD)(ocap - on), &got, NULL) && got)
    {
        on += got;
        if (on == ocap)
        {
            ocap *= 2;
            out = realloc(out, ocap + 1);
        }
    }
    out[on] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (status)
        *status = (int)code;
    return out;
}

int os_getpid(void)
{
    return (int)GetCurrentProcessId();
}

void os_random(void *out, size_t len)
{
    /* RtlGenRandom (SystemFunction036) is in advapi32 on XP and later */
    typedef BOOLEAN (WINAPI *gen_fn)(PVOID, ULONG);
    static gen_fn gen;
    if (!gen)
    {
        HMODULE m = LoadLibraryW(L"advapi32.dll");
        gen = m ? (gen_fn)(void (*)(void))GetProcAddress(m, "SystemFunction036") : NULL;
    }
    if (gen && gen(out, (ULONG)len))
        return;
    uint64_t x = (uint64_t)os_wall_ms() ^ (uint64_t)GetCurrentProcessId() << 32 ^ (uint64_t)(os_now() * 1e9);
    for (size_t n = 0; n < len; n++)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        ((unsigned char *)out)[n] = (unsigned char)x;
    }
}
