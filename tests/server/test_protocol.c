#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_commands_and_replies(void)
{
    const char command[] = "sEaRcH MixedCase.txt";
    const char invalid[] = "SEARCH name\0bad";
    assert(strcmp(protocol_command(command, sizeof(command) - 1), CMD_SEARCH) == 0);
    assert(protocol_command(invalid, sizeof(invalid) - 1) == NULL);
    assert(protocol_command("SEARCHING", sizeof("SEARCHING") - 1) == NULL);

    assert(protocol_status("200", 3) == STATUS_OK);
    assert(protocol_status("400", 3) == STATUS_BAD_REQUEST);
    assert(protocol_status("503", 3) == STATUS_UNAVAILABLE);
    const char *malformed[] = {"", "20", "200 ", "200 2", "530-Invalid",
                               "53x", "530\tInvalid", "600", "099"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
        assert(protocol_status(malformed[i], strlen(malformed[i])) == 0);
    assert(protocol_status("20\0", 3) == 0);
}

static void test_numbers(void)
{
    uint64_t value;
    assert(protocol_parse_uint64("0", UINT64_MAX, &value) && value == 0);
    assert(protocol_parse_uint64("00042", UINT64_MAX, &value) && value == 42);
    assert(protocol_parse_uint64("18446744073709551615", UINT64_MAX, &value) && value == UINT64_MAX);
    assert(protocol_parse_uint64("4294967295", UINT32_MAX, &value) && value == UINT32_MAX);
    assert(!protocol_parse_uint64("4294967296", UINT32_MAX, &value));
    const char *invalid[] = {"", "+1", "-1", "1x", " 1", "1 ",
                             "18446744073709551616", "999999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!protocol_parse_uint64(invalid[i], UINT64_MAX, &value));
}

static void test_filenames(void)
{
    char token[PROTOCOL_FILENAME_TOKEN_MAX + 1], decoded[PROTOCOL_FILENAME_MAX + 1];
    const char *names[] = {"notes.txt", "my notes.txt", "100%.txt", "a+b.txt",
                           "!#$&'()*,;:=?@[]{}", "b\xc3\xa1o c\xc3\xa1o.txt", "\xff"};
    const char *tokens[] = {"notes.txt", "my%20notes.txt", "100%25.txt", "a%2Bb.txt",
                            "%21%23%24%26%27%28%29%2A%2C%3B%3A%3D%3F%40%5B%5D%7B%7D",
                            "b%C3%A1o%20c%C3%A1o.txt", "%FF"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        assert(protocol_encode_filename(names[i], token, sizeof(token)));
        assert(strcmp(token, tokens[i]) == 0);
        assert(protocol_decode_filename(token, decoded, sizeof(decoded)));
        assert(strcmp(decoded, names[i]) == 0);
    }
    const char *invalid[] = {"", ".", "..", "a/b", "a\\b", "%2F", "%5C", "%00", "%0A",
                             "%7F", "%41", "%2E", "%2e", "%2b", "%", "%2", "%ZZ",
                             "a b", "a+b", "\xff"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!protocol_decode_filename(invalid[i], decoded, sizeof(decoded)));
    assert(!protocol_encode_filename("../file", token, sizeof(token)));
    assert(!protocol_encode_filename("\n", token, sizeof(token)));
    assert(!protocol_encode_filename("..", token, sizeof(token)));
    assert(!protocol_encode_filename("a b", token, 5));
    assert(protocol_encode_filename("a b", token, 6));
    assert(!protocol_decode_filename("a%20b", decoded, 3));
    assert(protocol_decode_filename("a%20b", decoded, 4));

    char longest[PROTOCOL_FILENAME_MAX + 2];
    memset(longest, ' ', PROTOCOL_FILENAME_MAX);
    longest[PROTOCOL_FILENAME_MAX] = '\0';
    assert(protocol_encode_filename(longest, token, sizeof(token)));
    assert(strlen(token) == PROTOCOL_FILENAME_TOKEN_MAX);
    assert(protocol_decode_filename(token, decoded, sizeof(decoded)));
    assert(strcmp(longest, decoded) == 0);
    longest[PROTOCOL_FILENAME_MAX] = 'x';
    longest[PROTOCOL_FILENAME_MAX + 1] = '\0';
    assert(!protocol_encode_filename(longest, token, sizeof(token)));
    memset(token, 'x', PROTOCOL_FILENAME_MAX + 1);
    token[PROTOCOL_FILENAME_MAX + 1] = '\0';
    assert(!protocol_decode_filename(token, decoded, sizeof(decoded)));
}

int main(void)
{
    test_commands_and_replies();
    test_numbers();
    test_filenames();
    puts("protocol tests passed");
    return 0;
}
