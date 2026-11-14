/*
 * Paint (C port) - the Open/Save pickers on Linux, and opening URLs
 *
 * First the XDG desktop portal's FileChooser, over a minimal D-Bus client of
 * our own: the session bus socket, AUTH EXTERNAL, Hello, one method call and
 * the Request's Response signal (the request's path is predicted from our
 * handle_token and matched before the call, so the answer cannot slip past).
 * Marshalling covers the types those messages use. Without a portal, zenity,
 * then kdialog. While any of them is open the window repaints from its back
 * buffer and the clipboard is served (x11_modal_wait); input waits.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include "host/x11_wire.h"
#include "os/os.h"
#include "ui/host.h"

typedef struct
{
    uint8_t *data;
    size_t len, cap;
} buf;

static void put(buf *b, const void *data, size_t len)
{
    if (b->len + len > b->cap)
    {
        b->cap = (b->len + len) * 2 + 256;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

static void put_text(buf *b, const char *text)
{
    put(b, text, strlen(text));
}

/* ---- D-Bus marshalling (little or big endian as the host is: the header says which) ---- */

static void align(buf *b, size_t to)
{
    static const uint8_t zeros[8];
    put(b, zeros, (to - b->len % to) % to);
}

static void put_u8(buf *b, uint8_t value)
{
    put(b, &value, 1);
}

static void put_u32(buf *b, uint32_t value)
{
    align(b, 4);
    put(b, &value, 4);
}

static void put_str(buf *b, const char *text)     /* s and o */
{
    put_u32(b, (uint32_t)strlen(text));
    put(b, text, strlen(text) + 1);
}

static void put_sig(buf *b, const char *sig)      /* g */
{
    put_u8(b, (uint8_t)strlen(sig));
    put(b, sig, strlen(sig) + 1);
}

/* An array: its length is filled in by end_array; elements start aligned to elem_align */
static size_t begin_array(buf *b, size_t elem_align)
{
    put_u32(b, 0);
    size_t at = b->len - 4;
    align(b, elem_align);
    return at;
}

static void end_array(buf *b, size_t at, size_t elem_align)
{
    size_t start = at + 4 + (elem_align - (at + 4) % elem_align) % elem_align;
    uint32_t len = (uint32_t)(b->len - start);
    memcpy(b->data + at, &len, 4);
}

/* One {sv} entry of an options dictionary: the key and the variant's signature (its value follows) */
static void put_option(buf *b, const char *key, const char *sig)
{
    align(b, 8);
    put_str(b, key);
    put_sig(b, sig);
}

static void put_header_field(buf *b, int code, const char *sig, const char *value)
{
    align(b, 8);
    put_u8(b, (uint8_t)code);
    put_sig(b, sig);
    if (sig[0] == 'g')
        put_sig(b, value);
    else
        put_str(b, value);
}

/* A method call's header; the body goes after it, then finish_message */
static void begin_call(buf *b, uint32_t serial, const char *dest, const char *path, const char *iface, const char *member,
                       const char *sig)
{
    uint16_t probe = 1;
    uint8_t fixed[12] = { *(uint8_t *)&probe ? 'l' : 'B', 1, 0, 1 };
    memcpy(fixed + 8, &serial, 4);
    b->len = 0;
    put(b, fixed, 12);
    size_t fields = begin_array(b, 8);
    put_header_field(b, 1, "o", path);
    put_header_field(b, 2, "s", iface);
    put_header_field(b, 3, "s", member);
    put_header_field(b, 6, "s", dest);
    if (sig && *sig)
        put_header_field(b, 8, "g", sig);
    end_array(b, fields, 8);
    align(b, 8);
}

static void finish_message(buf *b)
{
    size_t fields = 16 + *(uint32_t *)(b->data + 12);
    uint32_t body = (uint32_t)(b->len - ((fields + 7) & ~(size_t)7));
    memcpy(b->data + 4, &body, 4);
}

/* ---- reading messages ---- */

typedef struct
{
    const uint8_t *msg;
    size_t at, end;
} reader;

static void r_align(reader *r, size_t to)
{
    r->at += (to - r->at % to) % to;
}

static uint32_t r_u32(reader *r)
{
    uint32_t value = 0;
    r_align(r, 4);
    if (r->at + 4 <= r->end)
        memcpy(&value, r->msg + r->at, 4);
    r->at += 4;
    return value;
}

static const char *r_str(reader *r)
{
    uint32_t len = r_u32(r);
    const char *text = r->at + len < r->end ? (const char *)r->msg + r->at : "";
    r->at += (size_t)len + 1;
    return text;
}

static const char *r_sig(reader *r)
{
    size_t len = r->at < r->end ? r->msg[r->at] : 0;
    const char *sig = r->at + 1 + len < r->end ? (const char *)r->msg + r->at + 1 : "";
    r->at += 1 + len + 1;
    return sig;
}

/* The end of the one complete type that sig starts with */
static const char *type_end(const char *sig)
{
    if (*sig == 'a')
        return type_end(sig + 1);
    if (*sig == '(' || *sig == '{')
    {
        sig++;
        while (*sig && *sig != ')' && *sig != '}')
            sig = type_end(sig);
        return *sig ? sig + 1 : sig;
    }
    return *sig ? sig + 1 : sig;
}

static size_t type_align(char type)
{
    return type == 'y' || type == 'g' || type == 'v' ? 1 : type == 'n' || type == 'q' ? 2 :
           type == 'x' || type == 't' || type == 'd' || type == '(' || type == '{' ? 8 : 4;
}

/* Skip one value of the type sig starts with */
static void skip(reader *r, const char *sig)
{
    char type = *sig;
    r_align(r, type_align(type));
    if (type == 's' || type == 'o')
        r_str(r);
    else if (type == 'g')
        r_sig(r);
    else if (type == 'v')
    {
        const char *inner = r_sig(r);
        skip(r, inner);
    }
    else if (type == 'a')
    {
        uint32_t len = r_u32(r);
        r_align(r, type_align(sig[1]));
        r->at += len;
    }
    else if (type == '(' || type == '{')
    {
        for (const char *at = sig + 1; *at && *at != ')' && *at != '}' && r->at < r->end; at = type_end(at))
            skip(r, at);
    }
    else
        r->at += type_align(type) == 8 ? 8 : type == 'y' ? 1 : type_align(type);
}

typedef struct
{
    int type;
    uint32_t reply_serial;
    const char *path, *member, *sig;
    reader body;
} message;

/* The header of the message at the start of data, if it has all arrived: its length, else 0 */
static size_t parse_message(const uint8_t *data, size_t len, message *out)
{
    uint32_t body_len, fields_len;
    uint16_t probe = 1;
    if (len < 16)
        return 0;
    memcpy(&body_len, data + 4, 4);
    memcpy(&fields_len, data + 12, 4);
    size_t body = (16 + (size_t)fields_len + 7) & ~(size_t)7, total = body + body_len;
    if (len < total)
        return 0;
    *out = (message){ .type = data[1], .path = "", .member = "", .sig = "" };
    if (data[0] != (*(uint8_t *)&probe ? 'l' : 'B'))
    {
        out->type = 0;  /* (the other byte order: not one of the messages we wait for) */
        return total;
    }
    reader r = { data, 16, 16 + fields_len };
    while (r.at < r.end)
    {
        r_align(&r, 8);
        int code = r.msg[r.at++];
        const char *sig = r_sig(&r);
        reader value = r;
        if (code == 1)
            out->path = r_str(&value);
        else if (code == 3)
            out->member = r_str(&value);
        else if (code == 5)
            out->reply_serial = r_u32(&value);
        else if (code == 8)
            out->sig = r_sig(&value);
        skip(&r, sig);
    }
    out->body = (reader){ data, body, total };
    return total;
}

/* ---- the session bus ---- */

typedef struct
{
    int fd;
    uint32_t serial;
    buf in;
} bus;

static int bus_connect(bus *b)
{
    const char *env = getenv("DBUS_SESSION_BUS_ADDRESS"), *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[512];
    if (!env && runtime)
    {
        snprintf(fallback, sizeof(fallback), "unix:path=%s/bus", runtime);
        env = fallback;
    }
    for (const char *at = env; at && *at; at = strchr(at, ';') ? strchr(at, ';') + 1 : NULL)
    {   /* addresses separated by ';', each unix:path=... or unix:abstract=... */
        struct sockaddr_un addr = { .sun_family = AF_UNIX };
        const char *path = strstr(at, "path="), *abstract = strstr(at, "abstract="), *end = strchr(at, ';');
        const char *value = path && (!end || path < end) ? path + 5 : abstract && (!end || abstract < end) ? abstract + 9 : NULL;
        if (strncmp(at, "unix:", 5) || !value)
            continue;
        size_t len = strcspn(value, ",;");
        int offset = value == abstract + 9;     /* an abstract name starts with a NUL */
        if (len + (size_t)offset >= sizeof(addr.sun_path))
            continue;
        memcpy(addr.sun_path + offset, value, len);
        b->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)offset + len + !offset);
        if (b->fd >= 0 && connect(b->fd, (struct sockaddr *)&addr, size) == 0)
            return 0;
        if (b->fd >= 0)
            close(b->fd);
    }
    b->fd = -1;
    return -1;
}

static int send_all(int fd, const void *data, size_t len)
{
    while (len)
    {
        ssize_t put = write(fd, data, len);
        if (put < 0 && errno == EINTR)
            continue;
        if (put <= 0)
            return -1;
        data = (const uint8_t *)data + put;
        len -= (size_t)put;
    }
    return 0;
}

/* How long the bus itself may take to answer, and the portal to take our call (starting up
 * first, if it isn't running): past that there's no picker to be had from it. The user's
 * time in the picker has no limit. */
#define BUS_WAIT 1.0
#define PORTAL_WAIT 2.0

/* Wait for the bus to say something, until the time `until` (0: no limit); 1 when it has */
static int bus_wait(int fd, double until)
{
    int left = until ? (int)((until - os_now()) * 1000) : -1;
    return until && left <= 0 ? 0 : x11_modal_wait(fd, left);
}

/* Read more from the bus, waiting until the time `until` (0: no limit) */
static int bus_read(bus *b, double until)
{
    uint8_t chunk[4096];
    ssize_t got = bus_wait(b->fd, until) > 0 ? read(b->fd, chunk, sizeof(chunk)) : -1;
    if (got <= 0)
        return -1;
    put(&b->in, chunk, (size_t)got);
    return 0;
}

/* The next whole message (in b->in until bus_drop); -1 when the bus went away or `until` passed */
static int bus_next(bus *b, message *msg, size_t *len, double until)
{
    while (!(*len = parse_message(b->in.data, b->in.len, msg)))
    {
        if (bus_read(b, until))
            return -1;
    }
    return 0;
}

static void bus_drop(bus *b, size_t len)
{
    memmove(b->in.data, b->in.data + len, b->in.len - len);
    b->in.len -= len;
}

/* AUTH EXTERNAL with our uid (in hex of its decimal digits), then Hello: our unique name */
static int bus_open(bus *b, char *name, size_t cap)
{
    char line[256], uid[32], hex[64] = "";
    *b = (bus){ .fd = -1 };
    if (bus_connect(b))
        return -1;
    double until = os_now() + BUS_WAIT;
    snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
    for (size_t idx = 0; uid[idx]; idx++)
        snprintf(hex + 2 * idx, 3, "%02x", uid[idx]);
    int len = snprintf(line, sizeof(line), "%cAUTH EXTERNAL %s\r\n", 0, hex);
    size_t got = 0;
    if (send_all(b->fd, line, (size_t)len))
        return -1;
    while (got < sizeof(line) - 1 && (!got || line[got - 1] != '\n'))
    {
        if (bus_wait(b->fd, until) <= 0 || read(b->fd, line + got, 1) != 1)
            return -1;
        got++;
    }
    if (strncmp(line, "OK ", 3) || send_all(b->fd, "BEGIN\r\n", 7))
        return -1;
    buf msg = { 0 };
    begin_call(&msg, ++b->serial, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello", NULL);
    finish_message(&msg);
    int failed = send_all(b->fd, msg.data, msg.len);
    free(msg.data);
    for (message reply; !failed;)
    {
        size_t size;
        if (bus_next(b, &reply, &size, until))
            return -1;
        if (reply.reply_serial == b->serial)
        {
            snprintf(name, cap, "%s", reply.type == 2 ? r_str(&reply.body) : "");
            bus_drop(b, size);
            return reply.type == 2 ? 0 : -1;
        }
        bus_drop(b, size);
    }
    return -1;
}

static void bus_close(bus *b)
{
    if (b->fd >= 0)
        close(b->fd);
    free(b->in.data);
}

static void add_match(bus *b, const char *path)
{
    char rule[512];
    buf msg = { 0 };
    snprintf(rule, sizeof(rule), "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='%s'", path);
    begin_call(&msg, ++b->serial, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch", "s");
    put_str(&msg, rule);
    finish_message(&msg);
    send_all(b->fd, msg.data, msg.len);
    free(msg.data);
}

/* ---- the portal ---- */

/* filters a(sa(us)): each label with its glob patterns (type 0) */
static void put_filters(buf *m, const char *const *filters)
{
    put_option(m, "filters", "a(sa(us))");
    size_t list = begin_array(m, 8);
    for (int idx = 0; filters && filters[idx] && filters[idx + 1]; idx += 2)
    {
        align(m, 8);
        put_str(m, filters[idx]);
        size_t globs = begin_array(m, 8);
        for (const char *at = filters[idx + 1]; *at;)
        {
            size_t len = strcspn(at, ";");
            char glob[128];
            snprintf(glob, sizeof(glob), "%.*s", (int)len, at);
            align(m, 8);
            put_u32(m, 0);
            put_str(m, glob);
            at += len + (at[len] == ';');
        }
        end_array(m, globs, 8);
    }
    end_array(m, list, 8);
}

/* The first of the Response's uris, as a path (malloc'd), or NULL */
static char *response_path(message *msg)
{
    reader *r = &msg->body;
    if (r_u32(r) != 0)
        return NULL;    /* 1 cancelled, 2 failed */
    uint32_t len = r_u32(r);
    r_align(r, 8);
    size_t end = r->at + len;
    while (r->at < end && r->at < r->end)
    {
        r_align(r, 8);
        const char *key = r_str(r), *sig = r_sig(r);
        if (strcmp(key, "uris") || strcmp(sig, "as"))
        {
            skip(r, sig);
            continue;
        }
        if (r_u32(r) == 0)
            return NULL;
        const char *uri = r_str(r);
        return uri_to_path(uri, strlen(uri));
    }
    return NULL;
}

/* The pickers this session found missing, not to be tried again: without any, the
 * "type a path" dialog then comes at once */
enum
{
    LACK_PORTAL = 1,
    LACK_ZENITY = 2,
    LACK_KDIALOG = 4,
};
static int lacking;

/* 0 with *path set (NULL: cancelled), or -1 when there is no portal to ask */
static int portal_pick(int save, const char *title, const char *name, const char *const *filters, char **path)
{
    bus b;
    char unique[128], token[32], request[256], parent[32] = "";
    *path = NULL;
    if (bus_open(&b, unique, sizeof(unique)))
    {
        bus_close(&b);
        return -1;
    }
    uint32_t random;
    os_random(&random, sizeof(random));
    snprintf(token, sizeof(token), "paint%u", random);
    int at = snprintf(request, sizeof(request), "/org/freedesktop/portal/desktop/request/%s/%s", unique + 1, token);
    for (char *dot = request + at - (int)strlen(token) - (int)strlen(unique + 1) - 1; *dot && *dot != '/'; dot++)
        *dot = *dot == '.' ? '_' : *dot;     /* the sender ":1.42" becomes "1_42" */
    add_match(&b, request);
    if (x11_host_window())
        snprintf(parent, sizeof(parent), "x11:%x", x11_host_window());
    buf m = { 0 };
    begin_call(&m, ++b.serial, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
               "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile", "ssa{sv}");
    put_str(&m, parent);
    put_str(&m, title);
    size_t options = begin_array(&m, 8);
    put_option(&m, "handle_token", "s");
    put_str(&m, token);
    put_option(&m, "modal", "b");
    put_u32(&m, 1);
    if (filters && filters[0])
        put_filters(&m, filters);
    const char *slash = save && name ? strrchr(name, '/') : NULL;
    if (save && name)
    {
        put_option(&m, "current_name", "s");
        put_str(&m, slash ? slash + 1 : name);
    }
    if (slash)
    {   /* current_folder is a NUL-terminated byte string */
        put_option(&m, "current_folder", "ay");
        size_t bytes = begin_array(&m, 1);
        put(&m, name, (size_t)(slash - name));
        put_u8(&m, 0);
        end_array(&m, bytes, 1);
    }
    end_array(&m, options, 8);
    finish_message(&m);
    uint32_t call = b.serial;
    int failed = send_all(b.fd, m.data, m.len), result = -1;
    free(m.data);
    double until = os_now() + PORTAL_WAIT;     /* for the call's reply; then the picker is open */
    for (message msg; !failed;)
    {
        size_t size;
        if (bus_next(&b, &msg, &size, until))
            break;
        if (msg.reply_serial == call && msg.type == 3)
            break;  /* an error: no portal, or no file chooser in it */
        if (msg.reply_serial == call && msg.type == 2)
        {
            until = 0;
            const char *handle = r_str(&msg.body);
            if (strcmp(handle, request))
            {   /* an old portal that did not use our token */
                snprintf(request, sizeof(request), "%s", handle);
                add_match(&b, request);
            }
        }
        if (msg.type == 4 && !strcmp(msg.member, "Response") && !strcmp(msg.path, request))
        {
            *path = response_path(&msg);
            result = 0;
            break;
        }
        bus_drop(&b, size);
    }
    bus_close(&b);
    return result;
}

/* ---- zenity and kdialog ---- */

/* Run a picker and read its output while the window is kept alive: 0 with *out (NULL if cancelled),
 * or -1 when the program is not installed */
static int run_picker(char *const argv[], char **out)
{
    int fds[2], status = 0;
    buf text = { 0 };
    *out = NULL;
    if (pipe2(fds, O_CLOEXEC))
        return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(fds[1], 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    for (ssize_t got = 1; pid > 0 && got > 0;)
    {
        uint8_t chunk[1024];
        if (x11_modal_wait(fds[0], -1) < 0)
            break;
        got = read(fds[0], chunk, sizeof(chunk));
        if (got > 0)
            put(&text, chunk, (size_t)got);
        got = got < 0 && errno == EINTR ? 1 : got;
    }
    close(fds[0]);
    if (pid > 0)
        waitpid(pid, &status, 0);
    if (pid < 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 127))
    {
        free(text.data);
        return -1;
    }
    put(&text, "", 1);
    text.data[strcspn((char *)text.data, "\n")] = 0;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && text.data[0])
        *out = (char *)text.data;
    else
        free(text.data);
    return 0;
}

static char *dialog_pick(int save, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    char *args[48], *path = NULL;
    buf owned[20] = { { 0 } };
    int count = 0, n_owned = 0;
    /* zenity: --file-filter=Label | *.png *.jpg */
    args[count++] = "zenity";
    args[count++] = "--file-selection";
    put_text(&owned[n_owned], "--title=");
    put_text(&owned[n_owned], title);
    put(&owned[n_owned], "", 1);
    args[count++] = (char *)owned[n_owned++].data;
    if (save)
    {
        args[count++] = "--save";
        args[count++] = "--confirm-overwrite";
        put_text(&owned[n_owned], "--filename=");
        put_text(&owned[n_owned], name ? name : "");
        put(&owned[n_owned], "", 1);
        args[count++] = (char *)owned[n_owned++].data;
    }
    /* (kdialog's filter, built alongside: "*.png *.jpg|Label" lines) */
    buf kfilter = { 0 };
    for (int idx = 0; filters && filters[idx] && filters[idx + 1] && n_owned < 18; idx += 2)
    {
        buf *arg = &owned[n_owned++];
        put_text(arg, "--file-filter=");
        put_text(arg, filters[idx]);
        put_text(arg, " |");
        if (kfilter.len)
            put_text(&kfilter, "\n");
        for (const char *at = filters[idx + 1]; *at;)
        {
            size_t len = strcspn(at, ";");
            put_text(arg, " ");
            put(arg, at, len);
            put(&kfilter, at, len);
            at += len + (at[len] == ';');
            if (*at)
                put_text(&kfilter, " ");
        }
        put(arg, "", 1);
        put_text(&kfilter, "|");
        put_text(&kfilter, filters[idx]);
        args[count++] = (char *)arg->data;
    }
    put(&kfilter, "", 1);
    args[count] = NULL;
    int missing = (lacking & LACK_ZENITY) || run_picker(args, &path);
    if (missing)
    {
        lacking |= LACK_ZENITY;
        char *kdialog[] = { "kdialog", "--title", (char *)title, save ? "--getsavefilename" : "--getopenfilename",
                            save && name ? (char *)name : (char *)os_home_dir(), (char *)kfilter.data, NULL };
        missing = (lacking & LACK_KDIALOG) || run_picker(kdialog, &path);
        lacking |= missing ? LACK_KDIALOG : 0;
    }
    for (int idx = 0; idx < n_owned; idx++)
        free(owned[idx].data);
    free(kfilter.data);
    *unavailable = missing != 0;
    return path;
}

static char *pick(int save, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    char *path;
    int dummy;
    unavailable = unavailable ? unavailable : &dummy;
    *unavailable = 0;
    if (!(lacking & LACK_PORTAL) && portal_pick(save, title, name, filters, &path) == 0)
        return path;
    lacking |= LACK_PORTAL;
    return dialog_pick(save, title, name, filters, unavailable);
}

char *host_open_file(host_win *owner, const char *title, const char *const *filters, int *unavailable)
{
    return pick(0, title, NULL, filters, unavailable);
}

char *host_save_file(host_win *owner, const char *title, const char *name, const char *const *filters, int *unavailable)
{
    return pick(1, title, name, filters, unavailable);
}

/* xdg-open, in a grandchild so that nobody has to wait for it */
void host_open_url(const char *url)
{
    pid_t pid = fork();
    if (pid == 0)
    {
        if (fork() == 0)
        {
            int null = open("/dev/null", O_RDWR);
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}
