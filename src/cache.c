#include <errno.h>
#include <fcntl.h>
#include <stdcountof.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/limits.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "cache.h"

constexpr char CACHE_FILENAME[] = "data";
constexpr char METADATA_FILENAME[] = "metadata";
constexpr char TMP_CACHE_FILENAME[] = "data.tmp";
constexpr char TMP_METADATA_FILENAME[] = "metadata.tmp";

static int
get_cache_dir()
{
    const char *env_var_dir = getenv("CACHE_DIRECTORY");
    if (env_var_dir) {
        if (env_var_dir[0] == '\0') {
            (void)fprintf(stderr, "No cache directory set, does service file "
                                  "contain `CacheDirectory=`?\n");
            return -1;
        }

        return open(env_var_dir, O_PATH | O_CLOEXEC);
    }

    env_var_dir = getenv("XDG_CACHE_HOME");
    if (!env_var_dir || env_var_dir[0] == '\0') {
        const char *const home_dir = getenv("HOME");

        if (!home_dir || home_dir[0] == '\0') {
            (void)fprintf(stderr, "Can't locate cache directory, "
                                  "XDG_CACHE_HOME is empty or unset\n");
            return -1;
        }

        const int home_fd = open(home_dir, O_PATH | O_CLOEXEC);
        if (home_fd < 0) {
            perror("Failed to open $HOME");
            return -1;
        }

        if (mkdirat(home_fd, ".cache", S_IRWXU) < 0) {
            if (errno != EEXIST) {
                perror("Failed to create ~/.cache directory");
                close(home_fd);
                return -1;
            }
        }

        const int cache_fd = openat(home_fd, ".cache", O_PATH | O_CLOEXEC);
        close(home_fd);

        return cache_fd;
    }

    return open(env_var_dir, O_PATH | O_CLOEXEC);
}

struct cache_dir cache_open_dir()
{
    int cache_dir_fd = -1;
    bool persistence = true;
    const struct cache_dir err = { cache_dir_fd, persistence };

    int parent_cache_dir_fd = get_cache_dir();
    if (parent_cache_dir_fd < 0) {
        persistence = false;
        parent_cache_dir_fd = open(getenv("TMPDIR") ?: P_tmpdir, O_PATH | O_CLOEXEC);
        if (parent_cache_dir_fd < 0) {
            perror("Failed to open cache directory");
            return err;
        }
    }

    if (mkdirat(parent_cache_dir_fd, program_invocation_short_name, S_IRWXU) < 0) {
        if (errno != EEXIST) {
            perror("Failed to make cache subdirectory");
            close(parent_cache_dir_fd);
            return err;
        }
    }

    cache_dir_fd = openat(parent_cache_dir_fd, program_invocation_short_name,
                          O_PATH | O_CLOEXEC);
    if (cache_dir_fd < 0) {
        perror("Failed to open cache subdirectory");
    }
    close(parent_cache_dir_fd);

    return (struct cache_dir){ cache_dir_fd, persistence };
}

void *cache_open_file(const int dir_fd, size_t *const size)
{
    const int cache_fd = openat(dir_fd, CACHE_FILENAME, O_RDONLY);
    if (cache_fd < 0) {
        perror("Failed to open cache file");
        return nullptr;
    }

    struct stat statbuf;
    if (fstat(cache_fd, &statbuf) < 0) {
        perror("Failed to stat cache file");
        close(cache_fd);
        return nullptr;
    }
    *size = (size_t)statbuf.st_size;

    void *mem_buf = mmap(nullptr, *size, PROT_READ, MAP_PRIVATE, cache_fd, 0);
    if (mem_buf == MAP_FAILED) {
        perror("Failed to map cache file into memory");
        mem_buf = nullptr;
    }
    close(cache_fd);

    return mem_buf;
}

static int
commit_file(const int dir_fd, const int file_fd, const char *const tmp_filename,
            const char *const filename)
{
    if (fsync(file_fd) < 0) {
        perror("Failed to sync cache metadata to disk");
        return -1;
    }

    char proc_path[32];
    if (snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%i", file_fd) < 0) {
        perror("Failed to construct path for cache file");
        return -1;
    }

    if (unlinkat(dir_fd, tmp_filename, 0) < 0 && errno != ENOENT) {
        perror("Failed to unlink old tmp cache file");
        return -1;
    }

    if (linkat(AT_FDCWD, proc_path, dir_fd, tmp_filename,
               AT_SYMLINK_FOLLOW) < 0) {
        perror("Failed to link cache temporary file");
        return -1;
    }

    if (renameat(dir_fd, tmp_filename, dir_fd, filename) < 0) {
        perror("Failed to commit cache file");
        return -1;
    }

    return 0;
}

int cache_save(const int cache_dir_fd, const int cache_fd,
               const char *const filepath, const double frame_rate)
{
    const int metadata_fd =
        openat(cache_dir_fd, ".", O_TMPFILE | O_WRONLY, S_IRUSR | S_IWUSR);
    if (metadata_fd < 0) {
        perror("Failed to open cache metadata file for writing");
        return -1;
    }

    FILE *file = fdopen(metadata_fd, "w");
    if (!file) {
        perror("Failed to open cache metadata file for writing");
        close(metadata_fd);
        return -1;
    }

    int ret = 0;
    struct stat statbuf;
    if (stat(filepath, &statbuf) < 0) {
        perror("Failed to read media file");
        ret = -1;
        goto close;
    }

    if (fprintf(file, "%s %i\npath %s\nsize %ji\nmtime %ji %ji\nframerate %a",
                program_invocation_short_name, METADATA_VERSION, filepath,
                (intmax_t)statbuf.st_size, (intmax_t)statbuf.st_mtim.tv_sec,
                (intmax_t)statbuf.st_mtim.tv_nsec, frame_rate) < 0) {
        perror("Failed to write to cache metadata file");
        ret = -1;
        goto close;
    }

    if (fflush(file) < 0) {
        perror("Failed to flush user-space buffer");
        ret = -1;
        goto close;
    }

    if (fsync(metadata_fd) < 0) {
        perror("Failed to sync cache metadata to disk");
        ret = -1;
        goto close;
    }

    if (commit_file(cache_dir_fd, cache_fd, TMP_CACHE_FILENAME, CACHE_FILENAME) == 0) {
        ret = commit_file(cache_dir_fd, metadata_fd, TMP_METADATA_FILENAME,
                          METADATA_FILENAME);
    } else {
        ret = -1;
    }
close:

    if (fclose(file) < 0) {
        perror("Failed to close cache metadata file stream");
        return -1;
    }

    return ret;
}

enum { VERSION, PATH, SIZE, MTIME, FRAMERATE };

static int
parse_line(const unsigned int idx, const size_t nread, const char *const line,
           const char *const string[], const char *const path,
           double *const frame_rate, const struct stat *const statbuf)
{
    size_t len = strlen(string[idx]);
    if (nread <= len || memcmp(line, string[idx], len) != 0) return -1;

    const char *ptr = &line[len + 1];
    switch (idx) {
        case VERSION:
            return strtol(ptr, nullptr, 10) == METADATA_VERSION ? 0 : -1;
        case PATH:
            return memcmp(ptr, path, strcspn(path, "\n")) == 0 ? 0 : -1;
        case SIZE:
            return strtol(ptr, nullptr, 10) == statbuf->st_size ? 0 : -1;
        case MTIME:
            char *endptr;
            return strtol(ptr, &endptr, 10) == statbuf->st_mtim.tv_sec
                && strtol(endptr, nullptr, 10) == statbuf->st_mtim.tv_nsec ? 0 : -1;
        case FRAMERATE:
            *frame_rate = strtod(ptr, nullptr);
            return *frame_rate == 0 ? -1 : 0;
        default: return -1;
    }
}

bool cache_exists(const int dir_fd)
{
    if (faccessat(dir_fd, METADATA_FILENAME, R_OK, 0) < 0) {
        if (errno != ENOENT) perror("Failed to access cache metadata file");
        return false;
    }

    return true;
}

bool cache_valid(const int dir_fd, const char *const filepath,
                 double *const frame_rate)
{
    struct stat statbuf;
    if (stat(filepath, &statbuf) < 0) {
        perror("Failed to read media file");
        return false;
    }

    int fd = openat(dir_fd, METADATA_FILENAME, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open cache metadata file");
        return false;
    }

    FILE *file = fdopen(fd, "r");
    if (!file) {
        perror("Failed to open cache metadata file");
        close(fd);
        return false;
    }

    const char *const string[] = {
        [VERSION] = program_invocation_short_name,
        [PATH] = "path",
        [SIZE] = "size",
        [MTIME] = "mtime",
        [FRAMERATE] = "framerate"
    };

    size_t size;
    char *line = nullptr;
    unsigned int idx = 0;
    ssize_t nread;
    while ((nread = getline(&line, &size, file)) != EOF && idx < countof(string)) {
        if (ferror(file)) {
            perror("Failed to get line from cache metadata file");
            break;
        }

        if (parse_line(idx++, (size_t)nread, line, string, filepath, frame_rate,
                       &statbuf) < 0) break;
    }
    free(line);
    (void)fclose(file);

    const bool valid = idx == countof(string);

    return valid;
}
