#include "protocol.h"

#include <limits.h>
#include <string.h>

size_t protocol_tokens(char *line, size_t length, char **tokens, size_t capacity)
{
    if (length == 0 || capacity == 0 || line[0] == ' ' || line[length - 1] == ' ')
        return 0;
    size_t count = 1;
    tokens[0] = line;
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)line[i];
        if (byte < 0x20 || byte > 0x7e)
            return 0;
        if (byte == ' ') {
            if (count == capacity || line[i + 1] == ' ')
                return 0;
            tokens[count++] = line + i + 1;
        }
    }
    for (size_t i = 1; i < count; ++i)
        tokens[i][-1] = '\0';
    return count;
}

bool protocol_error(const char *line, size_t length)
{
    return length == 3 && (line[0] == '4' || line[0] == '5') &&
           line[1] >= '0' && line[1] <= '9' &&
           line[2] >= '0' && line[2] <= '9';
}

bool protocol_valid_username(const char *username)
{
    return username != NULL && *username != '\0' &&
           username[strspn(username, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_")] == '\0';
}

bool protocol_valid_password(const char *password)
{
    size_t length = strlen(password);
    if (length < PASSWORD_MIN || length > INT_MAX)
        return false;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)password[i] < 0x21 || (unsigned char)password[i] > 0x7e)
            return false;
    return true;
}

bool protocol_parse_uint64(const char *text, uint64_t maximum, uint64_t *value)
{
    uint64_t result = 0;
    if (text == NULL || value == NULL || *text == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10))
            return false;
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

static bool unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

static bool filename_byte(unsigned char c)
{
    return c >= 0x20 && c <= 0x7e && c != '/' && c != '\\';
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool protocol_encode_filename(const char *name, char *token, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    if (name == NULL || token == NULL || capacity == 0)
        return false;
    size_t length = strlen(name), used = 0;
    if (length == 0 || length > PROTOCOL_FILENAME_MAX ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!filename_byte(c))
            return false;
        size_t width = unreserved(c) ? 1U : 3U;
        if (width >= capacity - used)
            return false;
        if (width == 1) {
            token[used++] = (char)c;
        } else {
            token[used++] = '%';
            token[used++] = hex[c >> 4];
            token[used++] = hex[c & 15];
        }
    }
    token[used] = '\0';
    return true;
}

bool protocol_decode_filename(const char *token, char *name, size_t capacity)
{
    if (token == NULL || name == NULL || capacity == 0)
        return false;
    size_t used = 0;
    for (size_t i = 0; token[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)token[i];
        if (c == '%') {
            if (token[i + 1] == '\0' || token[i + 2] == '\0')
                return false;
            int high = hex_value((unsigned char)token[i + 1]);
            int low = hex_value((unsigned char)token[i + 2]);
            if (high < 0 || low < 0)
                return false;
            c = (unsigned char)(high * 16 + low);
            if (unreserved(c))
                return false;
            i += 2;
        } else if (!unreserved(c)) {
            return false;
        }
        if (!filename_byte(c) || used >= PROTOCOL_FILENAME_MAX || used >= capacity - 1)
            return false;
        name[used++] = (char)c;
    }
    name[used] = '\0';
    return used != 0 && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}
