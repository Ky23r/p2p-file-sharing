#include "protocol.h"

#include <limits.h>
#include <string.h>

static bool printable_ascii(const char *text, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)text[i];
        if (byte < 0x20 || byte > 0x7e)
            return false;
    }
    return true;
}

const char *protocol_command(const char *line, size_t length)
{
    static const char *const commands[] = {
        CMD_REGISTER, CMD_LOGIN, CMD_HELLO, CMD_LOGOUT, CMD_SHARE,
        CMD_UNSHARE, CMD_SEARCH, CMD_REPORT, CMD_GET
    };
    if (!printable_ascii(line, length))
        return NULL;
    const char *space = memchr(line, ' ', length);
    size_t verb_length = space == NULL ? length : (size_t)(space - line);
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        const char *command = commands[i];
        if (strlen(command) != verb_length)
            continue;
        size_t j = 0;
        for (; j < verb_length; ++j) {
            char letter = line[j];
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            if (letter != command[j])
                break;
        }
        if (j == verb_length)
            return command;
    }
    return NULL;
}

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

static bool final_reply(const char *line, size_t length)
{
    return length >= 5 && line[0] >= '1' && line[0] <= '5' &&
           line[1] >= '0' && line[1] <= '9' &&
           line[2] >= '0' && line[2] <= '9' &&
           line[3] == ' ' && line[4] != ' ' && line[length - 1] != ' ' &&
           printable_ascii(line + 4, length - 4);
}

bool protocol_reply_is(const char *line, size_t length, const char *code)
{
    return final_reply(line, length) && memcmp(line, code, 3) == 0;
}

bool protocol_error(const char *line, size_t length)
{
    return final_reply(line, length) && (line[0] == '4' || line[0] == '5');
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
    return c >= 0x20 && c != 0x7f && c != '/' && c != '\\';
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool protocol_valid_filename(const char *name)
{
    if (name == NULL || *name == '\0' || strlen(name) > PROTOCOL_FILENAME_MAX ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const unsigned char *byte = (const unsigned char *)name; *byte != '\0'; ++byte)
        if (!filename_byte(*byte))
            return false;
    return true;
}

bool protocol_encode_filename(const char *name, char *token, size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    if (token == NULL || capacity == 0 || !protocol_valid_filename(name))
        return false;
    size_t used = 0;
    for (const unsigned char *byte = (const unsigned char *)name; *byte != '\0'; ++byte) {
        unsigned char c = *byte;
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
