#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "artwork.h"

#define ARTWORK_MAX_BYTES (5 * 1024 * 1024)
#define HTTP_HEADER_MAX    (64 * 1024)
#define ARTWORK_FETCH_TIMEOUT_MS 5000
#define ARTWORK_DNS_TIMEOUT_MS   2000
#define ARTWORK_IMAGEPROXY_SIZE  512

typedef struct {
    char host[256];
    char port[8];
    char host_header[288];
    char path[4096];
} artwork_url_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool done;
    bool abandoned;
    int status;
    struct addrinfo *addresses;
    char host[256];
    char port[8];
} artwork_resolver_t;

static void artwork_error(char *error, size_t error_size, const char *fmt, ...)
{
    if (!error || !error_size) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

static bool artwork_detect_type(const uint8_t *data, size_t size,
                                char content_type[32])
{
    const char *mime = NULL;
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        mime = "image/jpeg";
    } else if (size >= 8 &&
               memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        mime = "image/png";
    } else if (size >= 6 &&
               (memcmp(data, "GIF87a", 6) == 0 ||
                memcmp(data, "GIF89a", 6) == 0)) {
        mime = "image/gif";
    } else if (size >= 12 && memcmp(data, "RIFF", 4) == 0 &&
               memcmp(data + 8, "WEBP", 4) == 0) {
        mime = "image/webp";
    }
    if (!mime) return false;
    snprintf(content_type, 32, "%s", mime);
    return true;
}

static bool artwork_normalize_imageproxy_path(char path[4096],
                                              char *error, size_t error_size)
{
    if (!strstr(path, "/imageproxy/")) return true;

    char normalized[4096];
    const char *query = strchr(path, '?');
    size_t base_len = query ? (size_t)(query - path) : strlen(path);
    if (base_len >= sizeof(normalized)) goto too_long;
    memcpy(normalized, path, base_len);
    size_t used = base_len;
    bool have_param = false;

    if (query) {
        const char *param = query + 1;
        while (*param) {
            const char *end = strchr(param, '&');
            if (!end) end = param + strlen(param);
            const char *equals = memchr(param, '=', (size_t)(end - param));
            size_t key_len = (size_t)((equals ? equals : end) - param);
            bool replace = (key_len == 4 && strncmp(param, "size", 4) == 0) ||
                           (key_len == 3 && strncmp(param, "fmt", 3) == 0);
            if (!replace && end > param) {
                size_t param_len = (size_t)(end - param);
                if (used + 1 + param_len >= sizeof(normalized)) goto too_long;
                normalized[used++] = have_param ? '&' : '?';
                memcpy(normalized + used, param, param_len);
                used += param_len;
                have_param = true;
            }
            param = *end ? end + 1 : end;
        }
    }

    int appended = snprintf(
        normalized + used, sizeof(normalized) - used,
        "%csize=%d&fmt=jpeg", have_param ? '&' : '?',
        ARTWORK_IMAGEPROXY_SIZE);
    if (appended <= 0 || (size_t)appended >= sizeof(normalized) - used)
        goto too_long;
    snprintf(path, 4096, "%s", normalized);
    return true;

too_long:
    artwork_error(error, error_size, "normalized imageproxy URL is too long");
    return false;
}

static bool artwork_parse_url(const char *url, artwork_url_t *parsed,
                              char *error, size_t error_size)
{
    if (strncmp(url, "http://", 7) != 0) {
        artwork_error(error, error_size,
                      "only local files and http:// artwork URLs are supported");
        return false;
    }
    const char *authority = url + 7;
    const char *path = strchr(authority, '/');
    const char *authority_end = path ? path : url + strlen(url);
    if (authority == authority_end ||
        memchr(authority, '@', (size_t)(authority_end - authority))) {
        artwork_error(error, error_size, "invalid artwork URL authority");
        return false;
    }

    const char *host_start = authority;
    const char *host_end = authority_end;
    const char *port_start = NULL;
    if (*host_start == '[') {
        const char *close = memchr(host_start, ']',
                                   (size_t)(authority_end - host_start));
        if (!close) {
            artwork_error(error, error_size, "invalid bracketed artwork host");
            return false;
        }
        host_start++;
        host_end = close;
        if (close + 1 < authority_end) {
            if (close[1] != ':') {
                artwork_error(error, error_size, "invalid artwork URL port");
                return false;
            }
            port_start = close + 2;
        }
    } else {
        const char *colon = memchr(authority, ':',
                                   (size_t)(authority_end - authority));
        if (colon) {
            host_end = colon;
            port_start = colon + 1;
        }
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (!host_len || host_len >= sizeof(parsed->host)) {
        artwork_error(error, error_size, "artwork URL host is too long");
        return false;
    }
    memcpy(parsed->host, host_start, host_len);
    parsed->host[host_len] = '\0';

    if (port_start) {
        size_t port_len = (size_t)(authority_end - port_start);
        if (!port_len || port_len >= sizeof(parsed->port)) {
            artwork_error(error, error_size, "invalid artwork URL port");
            return false;
        }
        for (size_t i = 0; i < port_len; i++) {
            if (port_start[i] < '0' || port_start[i] > '9') {
                artwork_error(error, error_size, "invalid artwork URL port");
                return false;
            }
        }
        memcpy(parsed->port, port_start, port_len);
        parsed->port[port_len] = '\0';
    } else {
        snprintf(parsed->port, sizeof(parsed->port), "80");
    }

    size_t authority_len = (size_t)(authority_end - authority);
    if (authority_len >= sizeof(parsed->host_header)) {
        artwork_error(error, error_size, "artwork URL authority is too long");
        return false;
    }
    memcpy(parsed->host_header, authority, authority_len);
    parsed->host_header[authority_len] = '\0';

    const char *request_path = path ? path : "/";
    size_t path_len = strcspn(request_path, "#");
    if (!path_len || path_len >= sizeof(parsed->path)) {
        artwork_error(error, error_size, "artwork URL path is too long");
        return false;
    }
    memcpy(parsed->path, request_path, path_len);
    parsed->path[path_len] = '\0';
    return artwork_normalize_imageproxy_path(parsed->path, error, error_size);
}

static int64_t artwork_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int artwork_remaining_ms(int64_t deadline_ms)
{
    int64_t remaining = deadline_ms - artwork_now_ms();
    if (remaining <= 0) return 0;
    return remaining > 0x7fffffff ? 0x7fffffff : (int)remaining;
}

static void *artwork_resolver_thread(void *arg)
{
    artwork_resolver_t *job = arg;
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int status = getaddrinfo(job->host, job->port, &hints, &addresses);

    pthread_mutex_lock(&job->lock);
    if (job->abandoned) {
        pthread_mutex_unlock(&job->lock);
        if (addresses) freeaddrinfo(addresses);
        pthread_cond_destroy(&job->ready);
        pthread_mutex_destroy(&job->lock);
        free(job);
        return NULL;
    }
    job->status = status;
    job->addresses = addresses;
    job->done = true;
    pthread_cond_signal(&job->ready);
    pthread_mutex_unlock(&job->lock);
    return NULL;
}

static bool artwork_resolve(const artwork_url_t *url,
                            struct addrinfo **addresses,
                            int64_t deadline_ms,
                            artwork_pump_cb pump, void *pump_arg,
                            char *error, size_t error_size)
{
    artwork_resolver_t *job = calloc(1, sizeof(*job));
    if (!job) {
        artwork_error(error, error_size, "out of memory resolving artwork host");
        return false;
    }
    pthread_mutex_init(&job->lock, NULL);
    pthread_cond_init(&job->ready, NULL);
    snprintf(job->host, sizeof(job->host), "%s", url->host);
    snprintf(job->port, sizeof(job->port), "%s", url->port);

    pthread_t thread;
    if (pthread_create(&thread, NULL, artwork_resolver_thread, job) != 0) {
        pthread_cond_destroy(&job->ready);
        pthread_mutex_destroy(&job->lock);
        free(job);
        artwork_error(error, error_size, "cannot start artwork DNS lookup");
        return false;
    }

    int64_t dns_deadline = artwork_now_ms() + ARTWORK_DNS_TIMEOUT_MS;
    if (dns_deadline > deadline_ms) dns_deadline = deadline_ms;
    while (true) {
        int remaining = artwork_remaining_ms(dns_deadline);
        pthread_mutex_lock(&job->lock);
        if (job->done) {
            pthread_mutex_unlock(&job->lock);
            break;
        }
        if (!remaining) {
            job->abandoned = true;
            pthread_mutex_unlock(&job->lock);
            pthread_detach(thread);
            artwork_error(error, error_size, "artwork DNS lookup timed out");
            return false;
        }
        int slice = remaining > 250 ? 250 : remaining;
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += (long)slice * 1000000L;
        if (timeout.tv_nsec >= 1000000000L) {
            timeout.tv_sec++;
            timeout.tv_nsec -= 1000000000L;
        }
        int wait_status = pthread_cond_timedwait(
            &job->ready, &job->lock, &timeout);
        bool done = job->done;
        pthread_mutex_unlock(&job->lock);
        if (done) break;
        if (wait_status != 0 && wait_status != ETIMEDOUT) {
            pthread_mutex_lock(&job->lock);
            if (job->done) {
                pthread_mutex_unlock(&job->lock);
                break;
            }
            job->abandoned = true;
            pthread_mutex_unlock(&job->lock);
            pthread_detach(thread);
            artwork_error(error, error_size, "artwork DNS lookup failed");
            return false;
        }
        if (pump) pump(pump_arg);
    }
    pthread_join(thread, NULL);

    bool ok = job->status == 0 && job->addresses;
    if (ok) {
        *addresses = job->addresses;
    } else {
        if (job->addresses) freeaddrinfo(job->addresses);
        artwork_error(error, error_size, "cannot resolve artwork host");
    }
    pthread_cond_destroy(&job->ready);
    pthread_mutex_destroy(&job->lock);
    free(job);
    return ok;
}

static bool artwork_wait_fd(int fd, short events, int64_t deadline_ms,
                            artwork_pump_cb pump, void *pump_arg)
{
    while (true) {
        int remaining = artwork_remaining_ms(deadline_ms);
        if (!remaining) return false;
        int slice = remaining > 250 ? 250 : remaining;
        struct pollfd pfd = {.fd = fd, .events = events};
        int status = poll(&pfd, 1, slice);
        if (status > 0) {
            if (pfd.revents & (POLLERR | POLLNVAL)) return false;
            if (pfd.revents & events) return true;
            if ((events & POLLIN) && (pfd.revents & POLLHUP)) return true;
            return false;
        }
        if (status < 0 && errno == EINTR) continue;
        if (status < 0) return false;
        if (pump) pump(pump_arg);
    }
}

static int artwork_connect(const artwork_url_t *url, int64_t deadline_ms,
                           artwork_pump_cb pump, void *pump_arg,
                           char *error, size_t error_size)
{
    struct addrinfo *addresses = NULL;
    if (!artwork_resolve(url, &addresses, deadline_ms, pump, pump_arg,
                         error, error_size))
        return -1;

    int fd = -1;
    for (struct addrinfo *addr = addresses; addr; addr = addr->ai_next) {
        if (!artwork_remaining_ms(deadline_ms)) break;
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int status = connect(fd, addr->ai_addr, addr->ai_addrlen);
        if (status == 0) break;
        if (errno == EINPROGRESS &&
            artwork_wait_fd(fd, POLLOUT, deadline_ms, pump, pump_arg)) {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                           &socket_error, &error_len) == 0 &&
                socket_error == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0)
        artwork_error(error, error_size, "cannot connect to artwork host");
    return fd;
}

static bool artwork_write_all(int fd, const uint8_t *data, size_t size,
                              int64_t deadline_ms,
                              artwork_pump_cb pump, void *pump_arg)
{
    size_t off = 0;
    while (off < size) {
        if (!artwork_wait_fd(fd, POLLOUT, deadline_ms, pump, pump_arg))
            return false;
#ifdef MSG_NOSIGNAL
        ssize_t written = send(fd, data + off, size - off, MSG_NOSIGNAL);
#else
        ssize_t written = send(fd, data + off, size - off, 0);
#endif
        if (written > 0) {
            off += (size_t)written;
        } else if (written < 0 &&
                   (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static size_t artwork_header_end(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 3 < size; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return i + 4;
    }
    return 0;
}

static bool artwork_header_value(const char *header, const char *name,
                                 char *out, size_t out_size)
{
    size_t name_len = strlen(name);
    const char *line = strstr(header, "\r\n");
    if (!line) return false;
    line += 2;
    while (*line) {
        const char *end = strstr(line, "\r\n");
        if (!end || end == line) break;
        size_t line_len = (size_t)(end - line);
        if (line_len > name_len && line[name_len] == ':' &&
            strncasecmp(line, name, name_len) == 0) {
            const char *value = line + name_len + 1;
            while (value < end && (*value == ' ' || *value == '\t')) value++;
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t value_len = (size_t)(end - value);
            if (value_len >= out_size) value_len = out_size - 1;
            memcpy(out, value, value_len);
            out[value_len] = '\0';
            return true;
        }
        line = end + 2;
    }
    return false;
}

static bool artwork_decode_chunked(const uint8_t *body, size_t body_size,
                                   uint8_t **decoded, size_t *decoded_size,
                                   char *error, size_t error_size)
{
    uint8_t *out = malloc(ARTWORK_MAX_BYTES);
    if (!out) {
        artwork_error(error, error_size, "out of memory decoding artwork");
        return false;
    }
    size_t in = 0, used = 0;
    while (in < body_size) {
        size_t line_end = in;
        while (line_end + 1 < body_size &&
               !(body[line_end] == '\r' && body[line_end + 1] == '\n'))
            line_end++;
        if (line_end + 1 >= body_size || line_end - in >= 32) goto malformed;

        char size_text[33];
        size_t size_len = line_end - in;
        memcpy(size_text, body + in, size_len);
        size_text[size_len] = '\0';
        char *extension = strchr(size_text, ';');
        if (extension) *extension = '\0';
        char *end = NULL;
        unsigned long chunk = strtoul(size_text, &end, 16);
        if (!end || end == size_text || *end != '\0') goto malformed;
        in = line_end + 2;
        if (chunk == 0) {
            *decoded = out;
            *decoded_size = used;
            return true;
        }
        if (chunk > ARTWORK_MAX_BYTES - used ||
            chunk > body_size - in ||
            in + chunk + 2 > body_size ||
            body[in + chunk] != '\r' || body[in + chunk + 1] != '\n')
            goto malformed;
        memcpy(out + used, body + in, chunk);
        used += chunk;
        in += chunk + 2;
    }

malformed:
    free(out);
    artwork_error(error, error_size, "malformed chunked artwork response");
    return false;
}

static bool artwork_fetch_http(const char *source, uint8_t **data, size_t *size,
                               char content_type[32],
                               artwork_pump_cb pump, void *pump_arg,
                               char *error, size_t error_size)
{
    artwork_url_t url = {0};
    if (!artwork_parse_url(source, &url, error, error_size)) return false;
    int64_t deadline_ms = artwork_now_ms() + ARTWORK_FETCH_TIMEOUT_MS;
    int fd = artwork_connect(&url, deadline_ms, pump, pump_arg,
                             error, error_size);
    if (fd < 0) return false;

    char request[4608];
    int request_len = snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: image/jpeg, image/png, image/*;q=0.8\r\n"
        "Connection: close\r\n"
        "User-Agent: cliairplay/0.1\r\n\r\n",
        url.path, url.host_header);
    if (request_len <= 0 || request_len >= (int)sizeof(request) ||
        !artwork_write_all(fd, (const uint8_t *)request,
                           (size_t)request_len, deadline_ms,
                           pump, pump_arg)) {
        artwork_error(error, error_size, "cannot send artwork request");
        close(fd);
        return false;
    }

    size_t cap = HTTP_HEADER_MAX;
    size_t used = 0;
    uint8_t *response = malloc(cap);
    bool ok = false;
    while (response) {
        if (used == cap) {
            if (cap >= ARTWORK_MAX_BYTES + HTTP_HEADER_MAX) break;
            size_t next = cap * 2;
            if (next > ARTWORK_MAX_BYTES + HTTP_HEADER_MAX)
                next = ARTWORK_MAX_BYTES + HTTP_HEADER_MAX;
            uint8_t *grown = realloc(response, next);
            if (!grown) break;
            response = grown;
            cap = next;
        }
        if (!artwork_wait_fd(fd, POLLIN, deadline_ms, pump, pump_arg)) break;
        ssize_t got = recv(fd, response + used, cap - used, 0);
        if (got > 0) {
            used += (size_t)got;
        } else if (got == 0) {
            ok = true;
            break;
        } else if (errno != EINTR && errno != EAGAIN &&
                   errno != EWOULDBLOCK) {
            break;
        }
    }
    close(fd);
    if (!ok || !response) {
        free(response);
        artwork_error(error, error_size, "incomplete artwork HTTP response");
        return false;
    }

    size_t header_len = artwork_header_end(response, used);
    if (!header_len || header_len >= HTTP_HEADER_MAX) {
        free(response);
        artwork_error(error, error_size, "invalid artwork HTTP response");
        return false;
    }
    char *header = malloc(header_len + 1);
    if (!header) {
        free(response);
        artwork_error(error, error_size, "out of memory parsing artwork");
        return false;
    }
    memcpy(header, response, header_len);
    header[header_len] = '\0';
    int status = 0;
    if (sscanf(header, "HTTP/%*s %d", &status) != 1 || status != 200) {
        free(header);
        free(response);
        artwork_error(error, error_size, "artwork HTTP status %d", status);
        return false;
    }

    uint8_t *body = response + header_len;
    size_t body_size = used - header_len;
    char value[128];
    uint8_t *image = NULL;
    size_t image_size = 0;
    if (artwork_header_value(header, "Transfer-Encoding",
                             value, sizeof(value)) &&
        strcasestr(value, "chunked")) {
        if (!artwork_decode_chunked(body, body_size, &image, &image_size,
                                    error, error_size)) {
            free(header);
            free(response);
            return false;
        }
    } else {
        if (artwork_header_value(header, "Content-Length",
                                 value, sizeof(value))) {
            char *end = NULL;
            unsigned long declared = strtoul(value, &end, 10);
            if (!end || end == value || *end != '\0' ||
                declared > ARTWORK_MAX_BYTES || declared > body_size) {
                free(header);
                free(response);
                artwork_error(error, error_size,
                              "invalid artwork Content-Length");
                return false;
            }
            body_size = (size_t)declared;
        }
        if (!body_size || body_size > ARTWORK_MAX_BYTES) {
            free(header);
            free(response);
            artwork_error(error, error_size, "artwork response is empty or too large");
            return false;
        }
        image = malloc(body_size);
        if (image) memcpy(image, body, body_size);
        image_size = body_size;
    }
    free(header);
    free(response);
    if (!image) {
        artwork_error(error, error_size, "out of memory loading artwork");
        return false;
    }
    if (!artwork_detect_type(image, image_size, content_type)) {
        free(image);
        artwork_error(error, error_size, "artwork response is not a supported image");
        return false;
    }
    *data = image;
    *size = image_size;
    return true;
}

static bool artwork_load_file(const char *path, uint8_t **data, size_t *size,
                              char content_type[32],
                              char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        artwork_error(error, error_size, "cannot open artwork file: %s",
                      strerror(errno));
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) goto read_error;
    long file_size = ftell(file);
    if (file_size <= 0 || file_size > ARTWORK_MAX_BYTES) {
        artwork_error(error, error_size, "artwork file is empty or too large");
        fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) goto read_error;
    uint8_t *image = malloc((size_t)file_size);
    if (!image) {
        artwork_error(error, error_size, "out of memory loading artwork");
        fclose(file);
        return false;
    }
    if (fread(image, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(image);
        goto read_error;
    }
    fclose(file);
    if (!artwork_detect_type(image, (size_t)file_size, content_type)) {
        free(image);
        artwork_error(error, error_size, "artwork file is not a supported image");
        return false;
    }
    *data = image;
    *size = (size_t)file_size;
    return true;

read_error:
    artwork_error(error, error_size, "cannot read artwork file");
    fclose(file);
    return false;
}

bool artwork_load(const char *source, uint8_t **data, size_t *size,
                  char content_type[32], artwork_pump_cb pump, void *pump_arg,
                  char *error, size_t error_size)
{
    if (!source || !*source || !data || !size || !content_type) {
        artwork_error(error, error_size, "invalid artwork source");
        return false;
    }
    *data = NULL;
    *size = 0;
    content_type[0] = '\0';
    if (strncmp(source, "http://", 7) == 0)
        return artwork_fetch_http(source, data, size, content_type,
                                  pump, pump_arg,
                                  error, error_size);
    if (strncmp(source, "https://", 8) == 0) {
        artwork_error(error, error_size,
                      "https artwork URLs are not supported; use MA's local imageproxy");
        return false;
    }
    return artwork_load_file(source, data, size, content_type,
                             error, error_size);
}
