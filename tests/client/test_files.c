#include "file_protocol.h"
#include "shared_index.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char encoded[FILE_NAME_HEX_CAP], decoded[FILE_NAME_CAP];
    const char *name = "b\xc3\xa1o c\xc3\xa1o.bin";
    assert(file_hex_encode(name, encoded, sizeof(encoded)));
    assert(file_name_decode(encoded, decoded));
    assert(strcmp(name, decoded) == 0);
    assert(!file_name_decode("2e2e", decoded));
    assert(!file_name_decode("612f62", decoded));
    assert(!file_name_decode("610062", decoded));
    assert(!file_name_decode("61FF", decoded));
    assert(!file_name_decode("1b", decoded));
    uint64_t number;
    assert(file_decimal("1099511627776", FILE_SIZE_MAX, &number));
    assert(number == FILE_SIZE_MAX);
    assert(!file_decimal("1099511627777", FILE_SIZE_MAX, &number));
    assert(!file_decimal("184467440737095516160", UINT64_MAX, &number));

    char directory[] = "/tmp/p2p-files-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char catalog[512], source[512], lock[520];
    snprintf(catalog, sizeof(catalog), "%s/index.txt", directory);
    snprintf(source, sizeof(source), "%s/%s", directory, name);
    snprintf(lock, sizeof(lock), "%s.lock", catalog);
    int fd = open(source, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0 && write(fd, "x\0\ny", 4) == 4);
    close(fd);
    shared_index *index = shared_index_load(catalog);
    assert(index != NULL);
    assert(shared_index_load(catalog) == NULL); /* One writer per catalog. */
    assert(shared_index_add(index, source, decoded) == 0);
    assert(strcmp(decoded, name) == 0);
    assert(shared_index_add(index, source, decoded) == 0); /* Idempotent local add. */
    shared_file entry;
    assert(shared_index_get(index, 0, &entry));
    assert(entry.size == 4 && !entry.announced);
    assert(shared_index_open(index, name, &number) < 0);
    shared_index_set_online(index, true);
    shared_index_mark(index, name, true, 4);
    fd = shared_index_open(index, name, &number);
    assert(fd >= 0 && number == 4);
    close(fd);
    shared_index_set_online(index, false);
    assert(shared_index_open(index, name, &number) < 0);
    shared_index_destroy(index);

    index = shared_index_load(catalog);
    assert(index != NULL && shared_index_get(index, 0, &entry));
    assert(!entry.announced && strcmp(entry.path, source) == 0);
    assert(shared_index_remove(index, name) == 0);
    assert(!shared_index_get(index, 0, &entry));
    shared_index_destroy(index);
    index = shared_index_load(catalog);
    assert(index != NULL && !shared_index_get(index, 0, &entry));
    shared_index_destroy(index);

    fd = open(catalog, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, "corrupt\n", 8) == 8);
    close(fd);
    assert(shared_index_load(catalog) == NULL);
    char preserved[8];
    fd = open(catalog, O_RDONLY);
    assert(fd >= 0 && read(fd, preserved, sizeof(preserved)) == 8);
    assert(memcmp(preserved, "corrupt\n", 8) == 0);
    close(fd);
    unlink(source);
    unlink(catalog);
    unlink(lock);
    rmdir(directory);
    puts("client files: encoding, limits, catalog persistence and authorization passed");
    return 0;
}
