#ifndef CACHE_H
#define CACHE_H

#include <fcntl.h>

constexpr unsigned int METADATA_VERSION = 1;

struct cache_dir {
    const int fd;
    const bool persistent;
};

struct cache_dir cache_open_dir();
bool cache_exists(int dir_fd);
void *cache_open_file(int dir_fd, size_t *size);
bool cache_valid(int dir_fd, const char *filepath, double *frame_rate);
int cache_save(int cache_dir_fd, int cache_fd, const char *filepath,
               double frame_rate);

#endif // CACHE_H
