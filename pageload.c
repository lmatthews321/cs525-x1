#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define READ_BUFFER_SIZE 8192
#define MAX_HEADER_SIZE 65536
#define HOST_NAME_SIZE 1025
#define SERVICE_NAME_SIZE 32

struct url {
    char *host;
    char *host_header;
    char *port;
    char *path;
};

struct trial_result {
    double dns_ms;
    double connect_ms;
    double first_byte_ms;
    double transfer_ms;
    size_t body_bytes;
    char resolved_address[HOST_NAME_SIZE];
    char local_address[HOST_NAME_SIZE];
    char local_port[SERVICE_NAME_SIZE];
    char remote_address[HOST_NAME_SIZE];
    char remote_port[SERVICE_NAME_SIZE];
    char status_line[512];
};

static void
print_usage(FILE *stream)
{
    fprintf(stream, "Usage: pageload [-4 | -6] [-n trials] [-o file] "
                    "http://host[:port][/path]\n");
}

static double
elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return ((double)(end->tv_sec - start->tv_sec) * 1000.0) +
           ((double)(end->tv_nsec - start->tv_nsec) / 1000000.0);
}

static int
get_time(struct timespec *time_value)
{
    return clock_gettime(CLOCK_MONOTONIC, time_value);
}

static void
free_url(struct url *url)
{
    free(url->host);
    free(url->host_header);
    free(url->port);
    free(url->path);
}

static int
parse_port(const char *text, char **port_out)
{
    char *end = NULL;
    long port;

    if (*text == '\0') {
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (!isdigit(*p)) {
            return -1;
        }
    }

    errno = 0;
    port = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || port < 1 || port > 65535) {
        return -1;
    }

    *port_out = strdup(text);
    return *port_out == NULL ? -1 : 0;
}

static int
parse_url(const char *input, struct url *url)
{
    const char *authority;
    const char *authority_end;
    const char *path_start;
    const char *host_start;
    const char *host_end;
    const char *port_start = NULL;
    size_t authority_length;
    size_t host_length;

    memset(url, 0, sizeof(*url));
    if (strncasecmp(input, "http://", 7) != 0) {
        fprintf(stderr, "pageload: URL must use the http:// scheme\n");
        return -1;
    }

    authority = input + 7;
    authority_end = authority + strcspn(authority, "/?#");
    path_start = authority_end;
    authority_length = (size_t)(authority_end - authority);
    if (authority_length == 0) {
        fprintf(stderr, "pageload: URL is missing a host\n");
        return -1;
    }
    if (strchr(authority, '@') != NULL && strchr(authority, '@') < authority_end) {
        fprintf(stderr, "pageload: user information is not supported in URLs\n");
        return -1;
    }
    for (const unsigned char *p = (const unsigned char *)input; *p != '\0'; ++p) {
        if (isspace(*p) || iscntrl(*p)) {
            fprintf(stderr, "pageload: URL must not contain whitespace or control characters\n");
            return -1;
        }
    }
    if (strchr(path_start, '#') != NULL) {
        fprintf(stderr, "pageload: URL fragments are not supported\n");
        return -1;
    }

    host_start = authority;
    if (*host_start == '[') {
        const char *closing_bracket = memchr(host_start, ']', authority_length);
        if (closing_bracket == NULL || closing_bracket == host_start + 1) {
            fprintf(stderr, "pageload: malformed bracketed IPv6 host\n");
            return -1;
        }
        host_start++;
        host_end = closing_bracket;
        if (closing_bracket + 1 < authority_end) {
            if (closing_bracket[1] != ':') {
                fprintf(stderr, "pageload: malformed URL authority\n");
                return -1;
            }
            port_start = closing_bracket + 2;
        }
    } else {
        const char *colon = memchr(authority, ':', authority_length);
        if (colon != NULL) {
            if (memchr(colon + 1, ':', (size_t)(authority_end - colon - 1)) != NULL) {
                fprintf(stderr, "pageload: IPv6 addresses in URLs must be bracketed\n");
                return -1;
            }
            host_end = colon;
            port_start = colon + 1;
        } else {
            host_end = authority_end;
        }
    }

    host_length = (size_t)(host_end - host_start);
    if (host_length == 0) {
        fprintf(stderr, "pageload: URL is missing a host\n");
        return -1;
    }
    url->host = malloc(host_length + 1);
    url->host_header = malloc(authority_length + 1);
    if (url->host == NULL || url->host_header == NULL) {
        fprintf(stderr, "pageload: out of memory while parsing URL\n");
        free_url(url);
        return -1;
    }
    memcpy(url->host, host_start, host_length);
    url->host[host_length] = '\0';
    memcpy(url->host_header, authority, authority_length);
    url->host_header[authority_length] = '\0';

    if (port_start == NULL) {
        url->port = strdup("80");
    } else {
        size_t port_length = (size_t)(authority_end - port_start);
        char *port_text = malloc(port_length + 1);
        int port_status;

        if (port_text == NULL) {
            fprintf(stderr, "pageload: out of memory while parsing URL\n");
            free_url(url);
            return -1;
        }
        memcpy(port_text, port_start, port_length);
        port_text[port_length] = '\0';
        port_status = parse_port(port_text, &url->port);
        free(port_text);
        if (port_status != 0) {
            fprintf(stderr, "pageload: port must be an integer from 1 to 65535\n");
            free_url(url);
            return -1;
        }
    }
    if (url->port == NULL) {
        fprintf(stderr, "pageload: out of memory while parsing URL\n");
        free_url(url);
        return -1;
    }

    if (*path_start == '\0') {
        url->path = strdup("/");
    } else if (*path_start == '?') {
        size_t query_length = strlen(path_start);
        url->path = malloc(query_length + 2);
        if (url->path != NULL) {
            url->path[0] = '/';
            memcpy(url->path + 1, path_start, query_length + 1);
        }
    } else {
        url->path = strdup(path_start);
    }
    if (url->path == NULL) {
        fprintf(stderr, "pageload: out of memory while parsing URL\n");
        free_url(url);
        return -1;
    }
    return 0;
}

static int
format_address(const struct sockaddr *address, socklen_t address_length,
               char *host, size_t host_length, char *service,
               size_t service_length)
{
    return getnameinfo(address, address_length, host, (socklen_t)host_length,
                       service, (socklen_t)service_length,
                       NI_NUMERICHOST | NI_NUMERICSERV);
}

static int
append_bytes(unsigned char **buffer, size_t *length, size_t *capacity,
             const unsigned char *data, size_t data_length)
{
    size_t required;
    size_t new_capacity;
    unsigned char *new_buffer;

    if (data_length > SIZE_MAX - *length) {
        return -1;
    }
    required = *length + data_length;
    if (required > *capacity) {
        new_capacity = *capacity == 0 ? READ_BUFFER_SIZE : *capacity;
        while (new_capacity < required) {
            if (new_capacity > SIZE_MAX / 2) {
                new_capacity = required;
                break;
            }
            new_capacity *= 2;
        }
        new_buffer = realloc(*buffer, new_capacity);
        if (new_buffer == NULL) {
            return -1;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    memcpy(*buffer + *length, data, data_length);
    *length = required;
    return 0;
}

static unsigned char *
find_header_end(unsigned char *response, size_t length)
{
    if (length < 4) {
        return NULL;
    }
    for (size_t i = 0; i <= length - 4; ++i) {
        if (memcmp(response + i, "\r\n\r\n", 4) == 0) {
            return response + i;
        }
    }
    return NULL;
}

static const unsigned char *
find_crlf(const unsigned char *data, size_t length)
{
    if (length < 2) {
        return NULL;
    }
    for (size_t i = 0; i + 1 < length; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

static int
is_chunked(const unsigned char *headers, size_t length)
{
    const unsigned char *line = headers;
    const unsigned char *end = headers + length;

    while (line < end) {
        const unsigned char *line_end = find_crlf(line, (size_t)(end - line));
        if (line_end == NULL) {
            line_end = end;
        }
        if ((size_t)(line_end - line) >= 18 &&
            strncasecmp((const char *)line, "Transfer-Encoding:", 18) == 0 &&
            line_end > line + 18) {
            const unsigned char *value = line + 18;
            size_t value_length = (size_t)(line_end - value);
            while (value_length > 0 && (*value == ' ' || *value == '\t')) {
                value++;
                value_length--;
            }
            for (size_t i = 0; i + 7 <= value_length; ++i) {
                if (strncasecmp((const char *)value + i, "chunked", 7) == 0 &&
                    (i == 0 || value[i - 1] == ',' || value[i - 1] == ' ' ||
                     value[i - 1] == '\t') &&
                    (i + 7 == value_length || value[i + 7] == ',' ||
                     value[i + 7] == ' ' || value[i + 7] == '\t')) {
                    return 1;
                }
            }
        }
        line = line_end + (line_end < end ? 2 : 0);
    }
    return 0;
}

enum chunk_state {
    CHUNK_SIZE_LINE,
    CHUNK_DATA,
    CHUNK_DATA_CR,
    CHUNK_DATA_LF,
    CHUNK_TRAILER_LINE,
    CHUNK_DONE
};

struct chunk_decoder {
    enum chunk_state state;
    size_t remaining;
    char line[READ_BUFFER_SIZE];
    size_t line_length;
    int line_cr;
};

static int
write_body(FILE *output, size_t *body_length, const unsigned char *data,
           size_t length)
{
    if (length > SIZE_MAX - *body_length) {
        fprintf(stderr, "pageload: response body is too large\n");
        return -1;
    }
    if (output != NULL && fwrite(data, 1, length, output) != length) {
        perror("pageload: writing output file");
        return -1;
    }
    *body_length += length;
    return 0;
}

static int
parse_chunk_size(const char *line, size_t length, size_t *chunk_size)
{
    size_t parsed = 0;
    size_t digits = 0;

    for (size_t i = 0; i < length && line[i] != ';'; ++i) {
        unsigned char c = (unsigned char)line[i];
        unsigned int value;

        if (c >= '0' && c <= '9') {
            value = (unsigned int)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value = (unsigned int)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value = (unsigned int)(c - 'A' + 10);
        } else {
            return -1;
        }
        if (parsed > (SIZE_MAX - value) / 16) {
            return -1;
        }
        parsed = parsed * 16 + value;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    *chunk_size = parsed;
    return 0;
}

static int
consume_chunked(struct chunk_decoder *decoder, const unsigned char *data,
                size_t length, FILE *output, size_t *body_length)
{
    size_t position = 0;

    while (position < length) {
        switch (decoder->state) {
        case CHUNK_SIZE_LINE:
        case CHUNK_TRAILER_LINE: {
            unsigned char c = data[position++];

            if (decoder->line_cr) {
                if (c != '\n') {
                    return -1;
                }
                decoder->line_cr = 0;
                if (decoder->state == CHUNK_SIZE_LINE) {
                    if (parse_chunk_size(decoder->line, decoder->line_length,
                                         &decoder->remaining) != 0) {
                        return -1;
                    }
                    decoder->state = decoder->remaining == 0
                                         ? CHUNK_TRAILER_LINE
                                         : CHUNK_DATA;
                } else if (decoder->line_length == 0) {
                    decoder->state = CHUNK_DONE;
                }
                decoder->line_length = 0;
            } else if (c == '\r') {
                decoder->line_cr = 1;
            } else if (c == '\n' || decoder->line_length == sizeof(decoder->line)) {
                return -1;
            } else {
                decoder->line[decoder->line_length++] = (char)c;
            }
            break;
        }
        case CHUNK_DATA: {
            size_t available = length - position;
            size_t amount = decoder->remaining < available
                                ? decoder->remaining
                                : available;

            if (write_body(output, body_length, data + position, amount) != 0) {
                return -2;
            }
            position += amount;
            decoder->remaining -= amount;
            if (decoder->remaining == 0) {
                decoder->state = CHUNK_DATA_CR;
            }
            break;
        }
        case CHUNK_DATA_CR:
            if (data[position++] != '\r') {
                return -1;
            }
            decoder->state = CHUNK_DATA_LF;
            break;
        case CHUNK_DATA_LF:
            if (data[position++] != '\n') {
                return -1;
            }
            decoder->state = CHUNK_SIZE_LINE;
            break;
        case CHUNK_DONE:
            return -1;
        }
    }
    return 0;
}

static int
valid_status_line(const char *line)
{
    const char *space;

    if (strncmp(line, "HTTP/", 5) != 0 ||
        !isdigit((unsigned char)line[5]) || line[6] != '.' ||
        !isdigit((unsigned char)line[7]) || line[8] != ' ') {
        return 0;
    }
    space = line + 9;
    if (!isdigit((unsigned char)space[0]) ||
        !isdigit((unsigned char)space[1]) ||
        !isdigit((unsigned char)space[2])) {
        return 0;
    }
    return space[3] == '\0' || space[3] == ' ';
}

static int
run_trial(const struct url *url, int family, int save_body,
          const char *output_path, struct trial_result *result)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    struct timespec start;
    struct timespec end;
    struct sockaddr_storage local;
    struct sockaddr_storage remote;
    socklen_t local_length = sizeof(local);
    socklen_t remote_length = sizeof(remote);
    char request[4096];
    int request_length;
    int socket_fd = -1;
    int status;
    unsigned char *response = NULL;
    size_t response_length = 0;
    size_t response_capacity = 0;
    unsigned char buffer[READ_BUFFER_SIZE];
    char resolved_service[SERVICE_NAME_SIZE];
    unsigned char *header_end;
    size_t body_length = 0;
    int chunked;
    struct chunk_decoder decoder = {CHUNK_SIZE_LINE, 0, {0}, 0, 0};
    struct timespec transfer_start = {0, 0};
    FILE *output = NULL;
    int rc = -1;

    memset(result, 0, sizeof(*result));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;

    if (get_time(&start) != 0) {
        perror("pageload: clock_gettime");
        return -1;
    }
    status = getaddrinfo(url->host, url->port, &hints, &addresses);
    if (get_time(&end) != 0) {
        perror("pageload: clock_gettime");
        goto cleanup;
    }
    result->dns_ms = elapsed_ms(&start, &end);
    if (status != 0) {
        fprintf(stderr, "pageload: DNS lookup for %s failed: %s\n",
                url->host, gai_strerror(status));
        goto cleanup;
    }
    address = addresses;
    if (address == NULL) {
        fprintf(stderr, "pageload: DNS lookup returned no addresses for %s\n", url->host);
        goto cleanup;
    }
    status = format_address(address->ai_addr, (socklen_t)address->ai_addrlen,
                            result->resolved_address,
                            sizeof(result->resolved_address), resolved_service,
                            sizeof(resolved_service));
    if (status != 0) {
        fprintf(stderr, "pageload: could not format resolved address: %s\n",
                gai_strerror(status));
        goto cleanup;
    }

    socket_fd = socket(address->ai_family, address->ai_socktype,
                       address->ai_protocol);
    if (socket_fd < 0) {
        perror("pageload: socket");
        goto cleanup;
    }
    if (get_time(&start) != 0) {
        perror("pageload: clock_gettime");
        goto cleanup;
    }
    status = connect(socket_fd, address->ai_addr, address->ai_addrlen);
    if (get_time(&end) != 0) {
        perror("pageload: clock_gettime");
        goto cleanup;
    }
    result->connect_ms = elapsed_ms(&start, &end);
    if (status != 0) {
        perror("pageload: connect");
        goto cleanup;
    }

    if (getsockname(socket_fd, (struct sockaddr *)&local, &local_length) != 0 ||
        getpeername(socket_fd, (struct sockaddr *)&remote, &remote_length) != 0) {
        perror("pageload: getsockname/getpeername");
        goto cleanup;
    }
    status = format_address((struct sockaddr *)&local, local_length,
                            result->local_address, sizeof(result->local_address),
                            result->local_port, sizeof(result->local_port));
    if (status == 0) {
        status = format_address((struct sockaddr *)&remote, remote_length,
                                result->remote_address,
                                sizeof(result->remote_address),
                                result->remote_port,
                                sizeof(result->remote_port));
    }
    if (status != 0) {
        fprintf(stderr, "pageload: could not format socket address: %s\n",
                gai_strerror(status));
        goto cleanup;
    }

    request_length = snprintf(request, sizeof(request),
                              "GET %s HTTP/1.1\r\nHost: %s\r\n"
                              "Connection: close\r\n\r\n",
                              url->path, url->host_header);
    if (request_length < 0 || (size_t)request_length >= sizeof(request)) {
        fprintf(stderr, "pageload: request is too long\n");
        goto cleanup;
    }
    if (save_body) {
        output = fopen(output_path, "wb");
        if (output == NULL) {
            perror("pageload: could not open output file");
            goto cleanup;
        }
    }

    if (get_time(&start) != 0) {
        perror("pageload: clock_gettime");
        goto cleanup;
    }
    {
        size_t sent = 0;
        while (sent < (size_t)request_length) {
            ssize_t amount = send(socket_fd, request + sent,
                                  (size_t)request_length - sent, 0);
            if (amount < 0 && errno == EINTR) {
                continue;
            }
            if (amount <= 0) {
                perror("pageload: send");
                goto cleanup;
            }
            sent += (size_t)amount;
        }
    }

    for (;;) {
        ssize_t amount = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            if (amount < 0) {
                perror("pageload: recv");
                goto cleanup;
            }
            fprintf(stderr, "pageload: connection closed before response headers\n");
            goto cleanup;
        }
        if (response_length == 0) {
            if (get_time(&end) != 0) {
                perror("pageload: clock_gettime");
                goto cleanup;
            }
            result->first_byte_ms = elapsed_ms(&start, &end);
            transfer_start = end;
        }
        if (append_bytes(&response, &response_length, &response_capacity,
                         buffer, (size_t)amount) != 0) {
            fprintf(stderr, "pageload: out of memory while reading response\n");
            goto cleanup;
        }
        header_end = find_header_end(response, response_length);
        if (header_end != NULL) {
            break;
        }
        if (response_length > MAX_HEADER_SIZE) {
            fprintf(stderr, "pageload: response headers are too large\n");
            goto cleanup;
        }
    }

    if ((size_t)(header_end - response) == 0 ||
        (size_t)(header_end - response) >= sizeof(result->status_line)) {
        fprintf(stderr, "pageload: invalid or overly long HTTP status line\n");
        goto cleanup;
    }
    {
        size_t status_length = 0;
        while (status_length < (size_t)(header_end - response) &&
               response[status_length] != '\r' && response[status_length] != '\n') {
            status_length++;
        }
        if (status_length == 0 || status_length >= sizeof(result->status_line)) {
            fprintf(stderr, "pageload: invalid HTTP status line\n");
            goto cleanup;
        }
        memcpy(result->status_line, response, status_length);
        result->status_line[status_length] = '\0';
    }
    if (!valid_status_line(result->status_line)) {
        fprintf(stderr, "pageload: malformed HTTP status line: %s\n",
                result->status_line);
        goto cleanup;
    }

    chunked = is_chunked(response, (size_t)(header_end - response));
    {
        size_t header_length = (size_t)(header_end - response) + 4;
        size_t initial_body_length = response_length - header_length;

        if (chunked) {
            status = consume_chunked(&decoder, response + header_length,
                                     initial_body_length, output, &body_length);
            if (status != 0) {
                if (status == -1) {
                    fprintf(stderr, "pageload: malformed chunked response\n");
                }
                goto cleanup;
            }
        } else if (write_body(output, &body_length, response + header_length,
                              initial_body_length) != 0) {
            goto cleanup;
        }
    }

    for (;;) {
        ssize_t amount = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount < 0) {
            perror("pageload: recv");
            goto cleanup;
        }
        if (amount == 0) {
            break;
        }
        if (chunked) {
            status = consume_chunked(&decoder, buffer, (size_t)amount, output,
                                     &body_length);
            if (status != 0) {
                if (status == -1) {
                    fprintf(stderr, "pageload: malformed chunked response\n");
                }
                goto cleanup;
            }
        } else if (write_body(output, &body_length, buffer,
                              (size_t)amount) != 0) {
            goto cleanup;
        }
    }
    if (chunked && decoder.state != CHUNK_DONE) {
        fprintf(stderr, "pageload: incomplete chunked response\n");
        goto cleanup;
    }
    if (get_time(&end) != 0) {
        perror("pageload: clock_gettime");
        goto cleanup;
    }
    result->transfer_ms = elapsed_ms(&transfer_start, &end);
    result->body_bytes = body_length;

    if (output != NULL && fclose(output) != 0) {
        output = NULL;
        perror("pageload: closing output file");
        goto cleanup;
    }
    output = NULL;
    rc = 0;

cleanup:
    if (output != NULL) {
        fclose(output);
    }
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    if (addresses != NULL) {
        freeaddrinfo(addresses);
    }
    free(response);
    return rc;
}

static int
compare_doubles(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

int
main(int argc, char **argv)
{
    struct url url;
    double *connect_times = NULL;
    struct trial_result result;
    const char *output_path = NULL;
    int family = AF_UNSPEC;
    int family_selected = 0;
    long trials = 1;
    int option;
    int exit_code = EXIT_FAILURE;

    while ((option = getopt(argc, argv, "46n:o:")) != -1) {
        switch (option) {
        case '4':
            if (family_selected) {
                fprintf(stderr, "pageload: choose only one of -4 or -6\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            family = AF_INET;
            family_selected = 1;
            break;
        case '6':
            if (family_selected) {
                fprintf(stderr, "pageload: choose only one of -4 or -6\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            family = AF_INET6;
            family_selected = 1;
            break;
        case 'n': {
            char *end = NULL;
            errno = 0;
            trials = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' || trials < 1) {
                fprintf(stderr, "pageload: trial count must be a positive integer\n");
                print_usage(stderr);
                return EXIT_FAILURE;
            }
            break;
        }
        case 'o':
            output_path = optarg;
            break;
        default:
            print_usage(stderr);
            return EXIT_FAILURE;
        }
    }
    if (optind + 1 != argc) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }
    if (parse_url(argv[optind], &url) != 0) {
        return EXIT_FAILURE;
    }
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("pageload: signal");
        free_url(&url);
        return EXIT_FAILURE;
    }

    if ((size_t)trials > SIZE_MAX / sizeof(*connect_times)) {
        fprintf(stderr, "pageload: trial count is too large\n");
        goto cleanup;
    }
    connect_times = malloc((size_t)trials * sizeof(*connect_times));
    if (connect_times == NULL) {
        fprintf(stderr, "pageload: out of memory for trial results\n");
        goto cleanup;
    }

    for (long i = 0; i < trials; ++i) {
        if (run_trial(&url, family, output_path != NULL && i == trials - 1,
                      output_path, &result) != 0) {
            fprintf(stderr, "pageload: trial %ld failed\n", i + 1);
            goto cleanup;
        }
        connect_times[i] = result.connect_ms;
        printf("Trial %ld\n", i + 1);
        printf("  DNS:        %.3f ms (%s -> %s)\n", result.dns_ms,
               url.host, result.resolved_address);
        printf("  Connect:    %.3f ms\n", result.connect_ms);
        printf("  TCP:        local %s:%s -> remote %s:%s\n",
               result.local_address, result.local_port,
               result.remote_address, result.remote_port);
        printf("  First byte: %.3f ms\n", result.first_byte_ms);
        printf("  Transfer:   %.3f ms, %zu body bytes, %.3f Mbit/s\n",
               result.transfer_ms, result.body_bytes,
               result.transfer_ms > 0.0
                   ? ((double)result.body_bytes * 8.0) /
                         (result.transfer_ms * 1000.0)
                   : 0.0);
        printf("  Status:     %s\n", result.status_line);
    }

    qsort(connect_times, (size_t)trials, sizeof(*connect_times), compare_doubles);
    printf("Minimum connect time: %.3f ms\n", connect_times[0]);
    if (trials % 2 == 1) {
        printf("Median connect time:  %.3f ms\n", connect_times[trials / 2]);
    } else {
        printf("Median connect time:  %.3f ms\n",
               (connect_times[trials / 2 - 1] + connect_times[trials / 2]) / 2.0);
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    free(connect_times);
    free_url(&url);
    return exit_code;
}
