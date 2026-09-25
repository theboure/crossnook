#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "sync/kosync.h"
#include "net/tlssimple.h"

#define KOSYNC_ACCEPT "application/vnd.koreader.v1+json"
#define KOSYNC_CONTENT_TYPE "application/json"

typedef struct json_writer {
    char *data;
    size_t cap;
    size_t len;
} json_writer;

typedef struct json_reader {
    const char *data;
    size_t len;
    size_t pos;
} json_reader;

enum parsed_field {
    FIELD_DOCUMENT = 1 << 0,
    FIELD_PROGRESS = 1 << 1,
    FIELD_PERCENTAGE = 1 << 2,
    FIELD_DEVICE = 1 << 3,
    FIELD_DEVICE_ID = 1 << 4,
    FIELD_TIMESTAMP = 1 << 5
};

typedef struct parsed_object {
    cn_kosync_progress progress;
    unsigned fields;
} parsed_object;

static size_t bounded_length(const char *text, size_t maximum)
{
    size_t length = 0;
    if (!text)
        return maximum + 1;
    while (length <= maximum && text[length] != '\0')
        ++length;
    return length;
}

static int header_text_ok(const char *text, size_t maximum, int allow_empty)
{
    size_t i;
    size_t length = bounded_length(text, maximum);
    if (length > maximum || (!allow_empty && length == 0))
        return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

static int document_id_ok(const char *document_id)
{
    size_t i;
    if (bounded_length(document_id, CN_KOSYNC_DOCUMENT_ID_BYTES) !=
        CN_KOSYNC_DOCUMENT_ID_BYTES)
        return 0;
    for (i = 0; i < CN_KOSYNC_DOCUMENT_ID_BYTES; ++i) {
        char c = document_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

static int utf8_valid(const char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i++];
        unsigned value;
        unsigned minimum;
        int remaining;
        if (c < 0x80) {
            if (c == 0)
                return 0;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            value = c & 0x1f;
            minimum = 0x80;
            remaining = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            value = c & 0x0f;
            minimum = 0x800;
            remaining = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            value = c & 0x07;
            minimum = 0x10000;
            remaining = 3;
        } else {
            return 0;
        }
        if (i + (size_t)remaining > length)
            return 0;
        while (remaining-- > 0) {
            unsigned char next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80)
                return 0;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff))
            return 0;
    }
    return 1;
}

void cn_kosync_progress_init(cn_kosync_progress *progress)
{
    if (!progress)
        return;
    memset(progress, 0, sizeof *progress);
    progress->progress_10000 = -1;
}

void cn_kosync_progress_clear(cn_kosync_progress *progress)
{
    if (!progress)
        return;
    free(progress->logical_position);
    cn_kosync_progress_init(progress);
}

cn_kosync_result cn_kosync_progress_set(cn_kosync_progress *progress,
                                        const char *document_id,
                                        const char *logical_position,
                                        int progress_10000,
                                        const char *device,
                                        const char *device_id)
{
    cn_kosync_progress replacement;
    size_t position_length;

    if (!progress || !document_id_ok(document_id) ||
        progress_10000 < 0 || progress_10000 > 10000 ||
        !header_text_ok(device, CN_KOSYNC_DEVICE_MAX, 0) ||
        !header_text_ok(device_id, CN_KOSYNC_DEVICE_MAX, 0))
        return CN_KOSYNC_INVALID;
    position_length = bounded_length(logical_position, CN_KOSYNC_POSITION_MAX);
    if (position_length == 0 || position_length > CN_KOSYNC_POSITION_MAX ||
        !utf8_valid(logical_position, position_length) ||
        !utf8_valid(device, strlen(device)) ||
        !utf8_valid(device_id, strlen(device_id)))
        return CN_KOSYNC_INVALID;

    cn_kosync_progress_init(&replacement);
    replacement.logical_position = (char *)malloc(position_length + 1);
    if (!replacement.logical_position)
        return CN_KOSYNC_NO_MEMORY;
    memcpy(replacement.logical_position, logical_position, position_length + 1);
    memcpy(replacement.document_id, document_id,
           CN_KOSYNC_DOCUMENT_ID_BYTES + 1);
    memcpy(replacement.device, device, strlen(device) + 1);
    memcpy(replacement.device_id, device_id, strlen(device_id) + 1);
    replacement.progress_10000 = progress_10000;
    cn_kosync_progress_clear(progress);
    *progress = replacement;
    return CN_KOSYNC_OK;
}

static cn_kosync_result copy_url_part(char *destination, size_t cap,
                                      const char *start, size_t length)
{
    if (length == 0 || length >= cap)
        return CN_KOSYNC_INVALID;
    memcpy(destination, start, length);
    destination[length] = '\0';
    return CN_KOSYNC_OK;
}

cn_kosync_result cn_kosync_client_init(cn_kosync_client *client,
                                       const char *base_url,
                                       const char *username,
                                       const char *userkey)
{
    const char *authority;
    const char *authority_end;
    const char *colon = NULL;
    const char *p;
    size_t path_length;
    int parsed_port;

    size_t scheme_len;
    int use_tls;

    if (!client || !base_url ||
        !header_text_ok(username, CN_KOSYNC_USERNAME_MAX, 0) ||
        !header_text_ok(userkey, CN_KOSYNC_USERKEY_MAX, 0))
        return CN_KOSYNC_INVALID;
    if (strncmp(base_url, "http://", 7) == 0) {
        scheme_len = 7;
        use_tls = 0;
    } else if (strncmp(base_url, "https://", 8) == 0) {
        scheme_len = 8;
        use_tls = 1;
    } else {
        return CN_KOSYNC_INVALID;
    }
    authority = base_url + scheme_len;
    authority_end = authority;
    while (*authority_end && *authority_end != '/') {
        if (*authority_end == ':' && !colon)
            colon = authority_end;
        if (*authority_end == '@' || *authority_end == '?' ||
            *authority_end == '#')
            return CN_KOSYNC_INVALID;
        ++authority_end;
    }
    if (authority == authority_end)
        return CN_KOSYNC_INVALID;

    memset(client, 0, sizeof *client);
    if (colon) {
        if (copy_url_part(client->host, sizeof client->host,
                          authority, (size_t)(colon - authority)) !=
                CN_KOSYNC_OK ||
            copy_url_part(client->port, sizeof client->port, colon + 1,
                          (size_t)(authority_end - colon - 1)) !=
                CN_KOSYNC_OK)
            return CN_KOSYNC_INVALID;
    } else {
        if (copy_url_part(client->host, sizeof client->host, authority,
                          (size_t)(authority_end - authority)) !=
            CN_KOSYNC_OK)
            return CN_KOSYNC_INVALID;
        strcpy(client->port, use_tls ? "443" : "80");
    }
    if (!cn_netsimple_parse_port(client->port, &parsed_port))
        return CN_KOSYNC_INVALID;
    (void)parsed_port;

    p = authority_end;
    path_length = strlen(p);
    if (strchr(p, '?') || strchr(p, '#') ||
        path_length > CN_KOSYNC_BASE_PATH_MAX)
        return CN_KOSYNC_INVALID;
    while (path_length > 0 && p[path_length - 1] == '/')
        --path_length;
    if (path_length != 0) {
        memcpy(client->base_path, p, path_length);
        client->base_path[path_length] = '\0';
    }
    if (!cn_netsimple_validate(client->host, client->port,
                               client->base_path[0] ?
                                   client->base_path : "/"))
        return CN_KOSYNC_INVALID;
    memcpy(client->username, username, strlen(username) + 1);
    memcpy(client->userkey, userkey, strlen(userkey) + 1);
    client->use_tls = use_tls;
    client->connect_ms = CN_NETSIMPLE_DEFAULT_CONNECT_MS;
    client->recv_ms = CN_NETSIMPLE_DEFAULT_RECV_MS;
    return CN_KOSYNC_OK;
}

cn_kosync_result cn_kosync_client_set_tls(
    cn_kosync_client *client, const struct cn_tls_config *tls,
    const char *connect_host)
{
    if (!client || !client->use_tls || !tls || !tls->ca_path)
        return CN_KOSYNC_INVALID;
    if (connect_host) {
        size_t length = bounded_length(connect_host, CN_NETSIMPLE_HOST_MAX);

        if (length == 0 || length > CN_NETSIMPLE_HOST_MAX ||
            !cn_netsimple_validate(connect_host, client->port, "/"))
            return CN_KOSYNC_INVALID;
        memcpy(client->connect_host, connect_host, length + 1);
    }
    client->tls = tls;
    return CN_KOSYNC_OK;
}

static int writer_bytes(json_writer *writer, const char *data, size_t length)
{
    if (!writer || !data || length > writer->cap - writer->len)
        return 0;
    memcpy(writer->data + writer->len, data, length);
    writer->len += length;
    return 1;
}

static int writer_text(json_writer *writer, const char *text)
{
    return writer_bytes(writer, text, strlen(text));
}

static int writer_string(json_writer *writer, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t length = strlen(text);
    if (!writer_text(writer, "\""))
        return 0;
    for (i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        const char *escape = NULL;
        switch (c) {
        case '\"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape) {
            if (!writer_text(writer, escape))
                return 0;
        } else if (c < 0x20) {
            char encoded[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            if (!writer_bytes(writer, encoded, sizeof encoded))
                return 0;
        } else if (!writer_bytes(writer, text + i, 1)) {
            return 0;
        }
    }
    return writer_text(writer, "\"");
}

static int writer_percentage(json_writer *writer, int progress_10000)
{
    char number[16];
    size_t length;
    if (progress_10000 == 10000)
        return writer_text(writer, "1");
    if (progress_10000 == 0)
        return writer_text(writer, "0");
    snprintf(number, sizeof number, "0.%04d", progress_10000);
    length = strlen(number);
    while (length > 2 && number[length - 1] == '0')
        number[--length] = '\0';
    return writer_text(writer, number);
}

cn_kosync_result cn_kosync_serialize_progress(
    const cn_kosync_progress *progress,
    char *json, size_t json_cap, size_t *json_len)
{
    json_writer writer;
    size_t position_length;
    if (!progress || !json || !json_len || json_cap == 0 ||
        !document_id_ok(progress->document_id) ||
        progress->progress_10000 < 0 || progress->progress_10000 > 10000 ||
        !header_text_ok(progress->device, CN_KOSYNC_DEVICE_MAX, 0) ||
        !header_text_ok(progress->device_id, CN_KOSYNC_DEVICE_MAX, 0) ||
        !utf8_valid(progress->device, strlen(progress->device)) ||
        !utf8_valid(progress->device_id, strlen(progress->device_id)))
        return CN_KOSYNC_INVALID;
    position_length = bounded_length(progress->logical_position,
                                     CN_KOSYNC_POSITION_MAX);
    if (position_length == 0 || position_length > CN_KOSYNC_POSITION_MAX ||
        !utf8_valid(progress->logical_position, position_length))
        return CN_KOSYNC_INVALID;
    writer.data = json;
    writer.cap = json_cap - 1;
    writer.len = 0;
    if (!writer_text(&writer, "{\"document\":") ||
        !writer_string(&writer, progress->document_id) ||
        !writer_text(&writer, ",\"progress\":") ||
        !writer_string(&writer, progress->logical_position) ||
        !writer_text(&writer, ",\"percentage\":") ||
        !writer_percentage(&writer, progress->progress_10000) ||
        !writer_text(&writer, ",\"device\":") ||
        !writer_string(&writer, progress->device) ||
        !writer_text(&writer, ",\"device_id\":") ||
        !writer_string(&writer, progress->device_id) ||
        !writer_text(&writer, "}"))
        return CN_KOSYNC_INVALID;
    json[writer.len] = '\0';
    *json_len = writer.len;
    return CN_KOSYNC_OK;
}

static void reader_space(json_reader *reader)
{
    while (reader->pos < reader->len &&
           (reader->data[reader->pos] == ' ' ||
            reader->data[reader->pos] == '\t' ||
            reader->data[reader->pos] == '\r' ||
            reader->data[reader->pos] == '\n'))
        ++reader->pos;
}

static int reader_take(json_reader *reader, char wanted)
{
    reader_space(reader);
    if (reader->pos >= reader->len || reader->data[reader->pos] != wanted)
        return 0;
    ++reader->pos;
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int reader_hex4(json_reader *reader, unsigned *value)
{
    int i;
    unsigned parsed = 0;
    if (reader->len - reader->pos < 4)
        return 0;
    for (i = 0; i < 4; ++i) {
        int digit = hex_value(reader->data[reader->pos++]);
        if (digit < 0)
            return 0;
        parsed = (parsed << 4) | (unsigned)digit;
    }
    *value = parsed;
    return 1;
}

static int append_codepoint(char *output, size_t cap, size_t *used,
                            unsigned value)
{
    unsigned char encoded[4];
    size_t count;
    if (value == 0 || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
        return 0;
    if (value < 0x80) {
        encoded[0] = (unsigned char)value; count = 1;
    } else if (value < 0x800) {
        encoded[0] = (unsigned char)(0xc0 | (value >> 6));
        encoded[1] = (unsigned char)(0x80 | (value & 0x3f)); count = 2;
    } else if (value < 0x10000) {
        encoded[0] = (unsigned char)(0xe0 | (value >> 12));
        encoded[1] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        encoded[2] = (unsigned char)(0x80 | (value & 0x3f)); count = 3;
    } else {
        encoded[0] = (unsigned char)(0xf0 | (value >> 18));
        encoded[1] = (unsigned char)(0x80 | ((value >> 12) & 0x3f));
        encoded[2] = (unsigned char)(0x80 | ((value >> 6) & 0x3f));
        encoded[3] = (unsigned char)(0x80 | (value & 0x3f)); count = 4;
    }
    if (count >= cap - *used)
        return 0;
    memcpy(output + *used, encoded, count);
    *used += count;
    return 1;
}

static int reader_string(json_reader *reader, char *output, size_t cap,
                         size_t *output_length)
{
    size_t used = 0;
    if (!output || cap == 0 || !reader_take(reader, '\"'))
        return 0;
    while (reader->pos < reader->len) {
        unsigned char c = (unsigned char)reader->data[reader->pos++];
        if (c == '\"') {
            output[used] = '\0';
            if (!utf8_valid(output, used))
                return 0;
            if (output_length)
                *output_length = used;
            return 1;
        }
        if (c < 0x20 || c == 0)
            return 0;
        if (c == '\\') {
            unsigned value;
            if (reader->pos >= reader->len)
                return 0;
            c = (unsigned char)reader->data[reader->pos++];
            switch (c) {
            case '\"': c = '\"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                if (!reader_hex4(reader, &value))
                    return 0;
                if (value >= 0xd800 && value <= 0xdbff) {
                    unsigned low;
                    if (reader->len - reader->pos < 6 ||
                        reader->data[reader->pos++] != '\\' ||
                        reader->data[reader->pos++] != 'u' ||
                        !reader_hex4(reader, &low) ||
                        low < 0xdc00 || low > 0xdfff)
                        return 0;
                    value = 0x10000 + ((value - 0xd800) << 10) +
                            (low - 0xdc00);
                }
                if (!append_codepoint(output, cap, &used, value))
                    return 0;
                continue;
            default:
                return 0;
            }
        }
        if (used + 1 >= cap)
            return 0;
        output[used++] = (char)c;
    }
    return 0;
}

static int number_token(json_reader *reader, char *output, size_t cap)
{
    size_t start;
    size_t length;
    reader_space(reader);
    start = reader->pos;
    if (reader->pos < reader->len && reader->data[reader->pos] == '-')
        ++reader->pos;
    if (reader->pos >= reader->len)
        return 0;
    if (reader->data[reader->pos] == '0') {
        ++reader->pos;
    } else if (reader->data[reader->pos] >= '1' &&
               reader->data[reader->pos] <= '9') {
        do {
            ++reader->pos;
        } while (reader->pos < reader->len &&
                 reader->data[reader->pos] >= '0' &&
                 reader->data[reader->pos] <= '9');
    } else {
        return 0;
    }
    if (reader->pos < reader->len && reader->data[reader->pos] == '.') {
        ++reader->pos;
        if (reader->pos >= reader->len || reader->data[reader->pos] < '0' ||
            reader->data[reader->pos] > '9')
            return 0;
        while (reader->pos < reader->len &&
               reader->data[reader->pos] >= '0' &&
               reader->data[reader->pos] <= '9')
            ++reader->pos;
    }
    if (reader->pos < reader->len &&
        (reader->data[reader->pos] == 'e' ||
         reader->data[reader->pos] == 'E')) {
        ++reader->pos;
        if (reader->pos < reader->len &&
            (reader->data[reader->pos] == '+' ||
             reader->data[reader->pos] == '-'))
            ++reader->pos;
        if (reader->pos >= reader->len || reader->data[reader->pos] < '0' ||
            reader->data[reader->pos] > '9')
            return 0;
        while (reader->pos < reader->len &&
               reader->data[reader->pos] >= '0' &&
               reader->data[reader->pos] <= '9')
            ++reader->pos;
    }
    length = reader->pos - start;
    if (length == 0 || length >= cap)
        return 0;
    memcpy(output, reader->data + start, length);
    output[length] = '\0';
    return 1;
}

static int reader_percentage(json_reader *reader, int *scaled)
{
    char token[64];
    char *end;
    double value;
    if (!number_token(reader, token, sizeof token))
        return 0;
    errno = 0;
    value = strtod(token, &end);
    if (errno || *end != '\0' || value < 0.0 || value > 1.0)
        return 0;
    *scaled = (int)(value * 10000.0 + 0.000000001);
    if (*scaled < 0 || *scaled > 10000)
        return 0;
    return 1;
}

static int reader_timestamp(json_reader *reader, long long *timestamp)
{
    char token[64];
    char *end;
    long long value;
    if (!number_token(reader, token, sizeof token))
        return 0;
    errno = 0;
    value = strtoll(token, &end, 10);
    if (errno || *end != '\0' || value < 0)
        return 0;
    *timestamp = value;
    return 1;
}

static int reader_skip_scalar(json_reader *reader)
{
    char token[64];
    char *end;
    reader_space(reader);
    if (reader->pos >= reader->len)
        return 0;
    if (reader->data[reader->pos] == '\"') {
        size_t cap = reader->len - reader->pos + 1;
        char *scratch = (char *)malloc(cap);
        int ok;
        if (!scratch)
            return 0;
        ok = reader_string(reader, scratch, cap, NULL);
        free(scratch);
        return ok;
    }
    if (reader->len - reader->pos >= 4 &&
        memcmp(reader->data + reader->pos, "null", 4) == 0) {
        reader->pos += 4; return 1;
    }
    if (reader->len - reader->pos >= 4 &&
        memcmp(reader->data + reader->pos, "true", 4) == 0) {
        reader->pos += 4; return 1;
    }
    if (reader->len - reader->pos >= 5 &&
        memcmp(reader->data + reader->pos, "false", 5) == 0) {
        reader->pos += 5; return 1;
    }
    if (!number_token(reader, token, sizeof token))
        return 0;
    errno = 0;
    (void)strtod(token, &end);
    return errno == 0 && *end == '\0';
}

static int set_once(unsigned *fields, unsigned field)
{
    if (*fields & field)
        return 0;
    *fields |= field;
    return 1;
}

static cn_kosync_result parse_object(const char *json, size_t json_len,
                                     parsed_object *parsed)
{
    json_reader reader;
    char key[32];
    char *position = NULL;
    int first = 1;

    if (!json || !parsed || json_len == 0 || json_len > CN_KOSYNC_JSON_MAX)
        return CN_KOSYNC_BAD_JSON;
    memset(parsed, 0, sizeof *parsed);
    cn_kosync_progress_init(&parsed->progress);
    reader.data = json;
    reader.len = json_len;
    reader.pos = 0;
    if (!reader_take(&reader, '{'))
        goto bad_json;
    reader_space(&reader);
    if (reader.pos < reader.len && reader.data[reader.pos] == '}') {
        ++reader.pos;
        reader_space(&reader);
        return reader.pos == reader.len ? CN_KOSYNC_OK : CN_KOSYNC_BAD_JSON;
    }
    while (reader.pos < reader.len) {
        if (!first && !reader_take(&reader, ','))
            goto bad_json;
        first = 0;
        if (!reader_string(&reader, key, sizeof key, NULL) ||
            !reader_take(&reader, ':'))
            goto bad_json;
        if (strcmp(key, "document") == 0) {
            if (!set_once(&parsed->fields, FIELD_DOCUMENT) ||
                !reader_string(&reader, parsed->progress.document_id,
                               sizeof parsed->progress.document_id, NULL))
                goto bad_json;
        } else if (strcmp(key, "progress") == 0) {
            size_t position_length;
            if (!set_once(&parsed->fields, FIELD_PROGRESS))
                goto bad_json;
            position = (char *)malloc(CN_KOSYNC_POSITION_MAX + 1);
            if (!position) {
                cn_kosync_progress_clear(&parsed->progress);
                return CN_KOSYNC_NO_MEMORY;
            }
            if (!reader_string(&reader, position,
                               CN_KOSYNC_POSITION_MAX + 1,
                               &position_length) || position_length == 0)
                goto bad_json;
            parsed->progress.logical_position = position;
            position = NULL;
        } else if (strcmp(key, "percentage") == 0) {
            if (!set_once(&parsed->fields, FIELD_PERCENTAGE) ||
                !reader_percentage(&reader,
                                   &parsed->progress.progress_10000))
                goto bad_json;
        } else if (strcmp(key, "device") == 0) {
            if (!set_once(&parsed->fields, FIELD_DEVICE) ||
                !reader_string(&reader, parsed->progress.device,
                               sizeof parsed->progress.device, NULL))
                goto bad_json;
        } else if (strcmp(key, "device_id") == 0) {
            if (!set_once(&parsed->fields, FIELD_DEVICE_ID) ||
                !reader_string(&reader, parsed->progress.device_id,
                               sizeof parsed->progress.device_id, NULL))
                goto bad_json;
        } else if (strcmp(key, "timestamp") == 0) {
            if (!set_once(&parsed->fields, FIELD_TIMESTAMP) ||
                !reader_timestamp(&reader, &parsed->progress.timestamp))
                goto bad_json;
            parsed->progress.has_timestamp = 1;
        } else if (!reader_skip_scalar(&reader)) {
            goto bad_json;
        }
        reader_space(&reader);
        if (reader.pos < reader.len && reader.data[reader.pos] == '}') {
            ++reader.pos;
            reader_space(&reader);
            if (reader.pos != reader.len)
                goto bad_json;
            return CN_KOSYNC_OK;
        }
    }

bad_json:
    free(position);
    cn_kosync_progress_clear(&parsed->progress);
    parsed->fields = 0;
    return CN_KOSYNC_BAD_JSON;
}

cn_kosync_result cn_kosync_parse_progress(const char *json, size_t json_len,
                                          cn_kosync_progress *progress)
{
    parsed_object parsed;
    cn_kosync_result result;
    unsigned required = FIELD_DOCUMENT | FIELD_PROGRESS | FIELD_PERCENTAGE |
                        FIELD_DEVICE;
    if (!progress)
        return CN_KOSYNC_INVALID;
    result = parse_object(json, json_len, &parsed);
    if (result != CN_KOSYNC_OK)
        return result;
    if (parsed.fields == 0) {
        cn_kosync_progress_clear(&parsed.progress);
        return CN_KOSYNC_NOT_FOUND;
    }
    if ((parsed.fields & required) != required ||
        !document_id_ok(parsed.progress.document_id) ||
        parsed.progress.device[0] == '\0') {
        cn_kosync_progress_clear(&parsed.progress);
        return CN_KOSYNC_BAD_PROTOCOL;
    }
    cn_kosync_progress_clear(progress);
    *progress = parsed.progress;
    return CN_KOSYNC_OK;
}

static cn_kosync_result endpoint_path(const cn_kosync_client *client,
                                      const char *suffix,
                                      char *path, size_t cap)
{
    int written;
    if (!client || !suffix || !path)
        return CN_KOSYNC_INVALID;
    written = snprintf(path, cap, "%s%s", client->base_path, suffix);
    if (written < 0 || (size_t)written >= cap)
        return CN_KOSYNC_INVALID;
    return CN_KOSYNC_OK;
}

static cn_kosync_result map_response(cn_netsimple_result transport,
                                     const cn_netsimple_response *response,
                                     cn_kosync_outcome *outcome)
{
    if (outcome) {
        outcome->transport_result = transport;
        outcome->http_status = transport == CN_NETSIMPLE_OK ?
                               response->status : 0;
    }
    if (transport != CN_NETSIMPLE_OK)
        return CN_KOSYNC_TRANSPORT_ERROR;
    if (response->status == 401)
        return CN_KOSYNC_AUTH_FAILED;
    if (response->status != 200)
        return CN_KOSYNC_HTTP_ERROR;
    return CN_KOSYNC_OK;
}

static void init_outcome(cn_kosync_outcome *outcome)
{
    if (!outcome)
        return;
    outcome->http_status = 0;
    outcome->transport_result = CN_NETSIMPLE_INVALID;
}

cn_kosync_result cn_kosync_authorize(const cn_kosync_client *client,
                                     cn_kosync_outcome *outcome)
{
    cn_netsimple_header headers[3];
    cn_netsimple_request request;
    cn_netsimple_response response;
    cn_netsimple_result transport;
    cn_kosync_result result;
    char path[CN_NETSIMPLE_PATH_MAX + 1];
    char *buffer;

    init_outcome(outcome);
    if (!client || (client->use_tls && !client->tls))
        return CN_KOSYNC_INVALID;
    result = endpoint_path(client, "/users/auth", path, sizeof path);
    if (result != CN_KOSYNC_OK)
        return result;
    buffer = (char *)malloc(CN_NETSIMPLE_RESPONSE_MAX);
    if (!buffer)
        return CN_KOSYNC_NO_MEMORY;
    headers[0].name = "Accept"; headers[0].value = KOSYNC_ACCEPT;
    headers[1].name = "x-auth-user"; headers[1].value = client->username;
    headers[2].name = "x-auth-key"; headers[2].value = client->userkey;
    memset(&request, 0, sizeof request);
    request.method = CN_NETSIMPLE_METHOD_GET;
    request.host = client->host; request.port = client->port;
    request.connect_host = client->connect_host[0] ? client->connect_host : NULL;
    request.path = path; request.headers = headers; request.header_count = 3;
    request.tls = client->use_tls ? client->tls : NULL;
    transport = cn_netsimple_exchange(&request, buffer,
                                      CN_NETSIMPLE_RESPONSE_MAX,
                                      client->connect_ms, client->recv_ms,
                                      &response);
    result = map_response(transport, &response, outcome);
    free(buffer);
    return result;
}

cn_kosync_result cn_kosync_put_progress(const cn_kosync_client *client,
                                        const cn_kosync_progress *progress,
                                        long long *server_timestamp,
                                        cn_kosync_outcome *outcome)
{
    cn_netsimple_header headers[3];
    cn_netsimple_request request;
    cn_netsimple_response response;
    parsed_object ack;
    cn_netsimple_result transport;
    cn_kosync_result result;
    char path[CN_NETSIMPLE_PATH_MAX + 1];
    char *json;
    char *buffer;
    size_t response_cap = CN_KOSYNC_JSON_MAX + CN_NETSIMPLE_REQUEST_MAX;
    size_t json_len;

    init_outcome(outcome);
    if (!client || !progress || (client->use_tls && !client->tls))
        return CN_KOSYNC_INVALID;
    json = (char *)malloc(CN_KOSYNC_JSON_MAX + 1);
    buffer = (char *)malloc(response_cap);
    if (!json || !buffer) {
        free(json); free(buffer);
        return CN_KOSYNC_NO_MEMORY;
    }
    result = cn_kosync_serialize_progress(progress, json,
                                          CN_KOSYNC_JSON_MAX + 1,
                                          &json_len);
    if (result != CN_KOSYNC_OK)
        goto done;
    result = endpoint_path(client, "/syncs/progress", path, sizeof path);
    if (result != CN_KOSYNC_OK)
        goto done;
    headers[0].name = "Accept"; headers[0].value = KOSYNC_ACCEPT;
    headers[1].name = "x-auth-user"; headers[1].value = client->username;
    headers[2].name = "x-auth-key"; headers[2].value = client->userkey;
    memset(&request, 0, sizeof request);
    request.method = CN_NETSIMPLE_METHOD_PUT;
    request.host = client->host; request.port = client->port;
    request.connect_host = client->connect_host[0] ? client->connect_host : NULL;
    request.path = path; request.headers = headers; request.header_count = 3;
    request.content_type = KOSYNC_CONTENT_TYPE;
    request.body = json; request.body_len = json_len;
    request.tls = client->use_tls ? client->tls : NULL;
    transport = cn_netsimple_exchange(&request, buffer, response_cap,
                                      client->connect_ms, client->recv_ms,
                                      &response);
    result = map_response(transport, &response, outcome);
    if (result != CN_KOSYNC_OK)
        goto done;
    result = parse_object(buffer + response.header_bytes,
                          response.body_bytes, &ack);
    if (result != CN_KOSYNC_OK)
        goto done;
    if ((ack.fields & (FIELD_DOCUMENT | FIELD_TIMESTAMP)) !=
            (FIELD_DOCUMENT | FIELD_TIMESTAMP) ||
        strcmp(ack.progress.document_id, progress->document_id) != 0) {
        cn_kosync_progress_clear(&ack.progress);
        result = CN_KOSYNC_BAD_PROTOCOL;
        goto done;
    }
    if (server_timestamp)
        *server_timestamp = ack.progress.timestamp;
    cn_kosync_progress_clear(&ack.progress);

done:
    free(json);
    free(buffer);
    return result;
}

cn_kosync_result cn_kosync_get_progress(const cn_kosync_client *client,
                                        const char *document_id,
                                        cn_kosync_progress *progress,
                                        cn_kosync_outcome *outcome)
{
    cn_netsimple_header headers[3];
    cn_netsimple_request request;
    cn_netsimple_response response;
    cn_netsimple_result transport;
    cn_kosync_result result;
    char suffix[64];
    char path[CN_NETSIMPLE_PATH_MAX + 1];
    char *buffer;
    size_t response_cap = CN_KOSYNC_JSON_MAX + CN_NETSIMPLE_REQUEST_MAX;

    init_outcome(outcome);
    if (!client || !progress || !document_id_ok(document_id) ||
        (client->use_tls && !client->tls))
        return CN_KOSYNC_INVALID;
    if (snprintf(suffix, sizeof suffix, "/syncs/progress/%s",
                 document_id) >= (int)sizeof suffix ||
        endpoint_path(client, suffix, path, sizeof path) != CN_KOSYNC_OK)
        return CN_KOSYNC_INVALID;
    buffer = (char *)malloc(response_cap);
    if (!buffer)
        return CN_KOSYNC_NO_MEMORY;
    headers[0].name = "Accept"; headers[0].value = KOSYNC_ACCEPT;
    headers[1].name = "x-auth-user"; headers[1].value = client->username;
    headers[2].name = "x-auth-key"; headers[2].value = client->userkey;
    memset(&request, 0, sizeof request);
    request.method = CN_NETSIMPLE_METHOD_GET;
    request.host = client->host; request.port = client->port;
    request.connect_host = client->connect_host[0] ? client->connect_host : NULL;
    request.path = path; request.headers = headers; request.header_count = 3;
    request.tls = client->use_tls ? client->tls : NULL;
    transport = cn_netsimple_exchange(&request, buffer, response_cap,
                                      client->connect_ms, client->recv_ms,
                                      &response);
    result = map_response(transport, &response, outcome);
    if (result == CN_KOSYNC_OK) {
        result = cn_kosync_parse_progress(buffer + response.header_bytes,
                                          response.body_bytes, progress);
        if (result == CN_KOSYNC_OK &&
            strcmp(progress->document_id, document_id) != 0) {
            cn_kosync_progress_clear(progress);
            result = CN_KOSYNC_BAD_PROTOCOL;
        }
    }
    free(buffer);
    return result;
}

cn_kosync_remote_state cn_kosync_classify_remote(
    const cn_kosync_progress *remote,
    const char *local_position, int local_progress_10000,
    long long local_timestamp,
    const char *local_device_model, const char *local_device_id)
{
    if (remote && local_device_model && local_device_id &&
        strcmp(remote->device, local_device_model) == 0 &&
        strcmp(remote->device_id, local_device_id) == 0)
        return CN_KOSYNC_REMOTE_SAME_DEVICE;
    if (remote && local_position &&
        (remote->progress_10000 == local_progress_10000 ||
         (remote->logical_position &&
          strcmp(remote->logical_position, local_position) == 0)))
        return CN_KOSYNC_REMOTE_ALREADY_SYNCED;
    if (remote && ((remote->has_timestamp &&
                    remote->timestamp > local_timestamp) ||
                   (!remote->has_timestamp &&
                    remote->progress_10000 > local_progress_10000)))
        return CN_KOSYNC_REMOTE_NEWER;
    return CN_KOSYNC_REMOTE_OLDER_OR_EQUAL;
}

const char *cn_kosync_result_name(cn_kosync_result result)
{
    static const char *const names[] = {
        "ok", "not-found", "invalid", "auth-failed", "http-error",
        "transport-error", "bad-json", "bad-protocol", "no-memory"
    };
    if ((size_t)result < CN_KOSYNC_RESULT_COUNT)
        return names[result];
    return "unknown";
}
