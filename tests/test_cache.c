#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdcountof.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#include "../src/cache.h"

static unsigned int tests_run = 0;
static unsigned int tests_failed = 0;

static bool current_test_failed = false;

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            current_test_failed = true;                                        \
            return;                                                            \
        }                                                                      \
    } while (false)

#define TEST_DEF(test) static void test_##test(const struct fixture *fx)

#define TEST(name) { #name, test_##name }

struct fixture {
    char *const tmp_dir;
    const int dir_fd;

    struct {
        const char *const name;
        char *const fullpath;
        const size_t size;
        const double frame_rate;

        const int fd;
        uint8_t *bytes;
    } media;
};

static void
save_cache(const struct fixture *fx)
{
    char filepath[PATH_MAX];
    assert(snprintf(filepath, sizeof(filepath), "%s/%s", fx->tmp_dir, fx->media.name) > 0);

    ASSERT(cache_save(fx->dir_fd, fx->media.fd, filepath, fx->media.frame_rate) == 0);
}

TEST_DEF(cache_save)
{
    save_cache(fx);
}

TEST_DEF(cache_data_exists)
{
    save_cache(fx);
    ASSERT(faccessat(fx->dir_fd, "data", F_OK, 0) == 0);
    ASSERT(faccessat(fx->dir_fd, "metadata", F_OK, 0) == 0);
}

TEST_DEF(cache_existence_check)
{
    save_cache(fx);
    ASSERT(cache_exists(fx->dir_fd));
}

TEST_DEF(cache_validation)
{
    save_cache(fx);
    char filepath[PATH_MAX];
    assert(snprintf(filepath, sizeof(filepath), "%s/%s", fx->tmp_dir, fx->media.name) > 0);

    double fps_ptr = 0;
    ASSERT(cache_valid(fx->dir_fd, filepath, &fps_ptr));

    ASSERT(fps_ptr == fx->media.frame_rate);
}

TEST_DEF(media_mtime_changed)
{
    save_cache(fx);
    assert(futimens(fx->media.fd, (const struct timespec[]){
        {},
        { .tv_sec = 42069 }
    }) == 0);
    ASSERT(!cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));
}

TEST_DEF(media_size_changed)
{
    save_cache(fx);
    assert(ftruncate(fx->media.fd, (off_t)fx->media.size - 69) == 0);
    ASSERT(!cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));
}

TEST_DEF(media_path_changed)
{
    save_cache(fx);
    constexpr char filename[] = "different_media.mp4";

    char fullpath[PATH_MAX];
    assert(snprintf(fullpath, sizeof(fullpath), "%s/%s", fx->tmp_dir, filename) > 0);

    const int media_fd = open(fullpath, O_CREAT, S_IRUSR);
    assert(media_fd >= 0);

    ASSERT(!cache_valid(fx->dir_fd, fullpath, &(double){}));
}

TEST_DEF(invalid_metadata_version)
{
    save_cache(fx);
    const int metadata_fd = openat(fx->dir_fd, "metadata", O_RDWR);
    assert(metadata_fd >= 0);
    const char metadata_line[] = "trccd wrong_version_here\n";
    const ssize_t nwrit = write(metadata_fd, metadata_line, sizeof(metadata_line) - 1);
    assert(nwrit == sizeof(metadata_line) - 1);

    ASSERT(!cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));

    close(metadata_fd);
}

TEST_DEF(empty_metadata_file)
{
    save_cache(fx);
    const int metadata_fd = openat(fx->dir_fd, "metadata", O_RDWR);
    assert(metadata_fd >= 0);
    assert(ftruncate(metadata_fd, 0) == 0);

    ASSERT(!cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));

    close(metadata_fd);
}

TEST_DEF(corrupt_metadata_file)
{
    save_cache(fx);
    const int metadata_fd = openat(fx->dir_fd, "metadata", O_RDWR);
    assert(metadata_fd >= 0);

    struct stat stat;
    assert(fstat(metadata_fd, &stat) == 0);
    size_t size = (size_t)stat.st_size;

    uint8_t *corrupted_metadata =
        mmap(nullptr, size, PROT_WRITE | PROT_READ, MAP_SHARED,
             metadata_fd, 0);
    close(metadata_fd);
    assert(corrupted_metadata != MAP_FAILED);

    srandom(42069);
    for (size_t i = 0; i + sizeof(long) <= size; i += sizeof(long)) {
        long *ptr = (long*)&corrupted_metadata[i];
        *ptr = random();
    }

    ASSERT(!cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));

    munmap(corrupted_metadata, (size_t)stat.st_size);
}

TEST_DEF(stale_tmp_does_not_block_save)
{
    const int data_tmp_fd =
        openat(fx->dir_fd, "data.tmp", O_CREAT | O_RDWR, S_IRUSR);
    assert(data_tmp_fd >= 0);
    const int metadata_tmp_fd =
        openat(fx->dir_fd, "metadata.tmp", O_CREAT | O_RDWR, S_IRUSR);
    assert(metadata_tmp_fd >= 0);

    save_cache(fx);

    ASSERT(cache_valid(fx->dir_fd, fx->media.fullpath, &(double){}));

    close(data_tmp_fd);
    close(metadata_tmp_fd);
}

struct fixture
create_fixture(void)
{
    char template[PATH_MAX];
    const int ret = snprintf(template, sizeof(template), "%s/XXXXXX",
                             getenv("TMPDIR") ?: P_tmpdir);
    assert(ret > 0 && (size_t)ret < sizeof(template));
    const char *const tmp_dir = mkdtemp(template);
    assert(tmp_dir);

    const int cache_dir_fd = open(tmp_dir, O_PATH);
    assert(cache_dir_fd >= 0);

    const char *const filename = "test_media.mkv";
    constexpr size_t filesize = 42069;

    const int fake_media_fd =
        openat(cache_dir_fd, filename, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    assert(fake_media_fd >= 0);
    assert(ftruncate(fake_media_fd, filesize) == 0);

    uint8_t *fake_bytes = mmap(nullptr, filesize, PROT_WRITE | PROT_READ, MAP_SHARED, fake_media_fd, 0);
    assert(fake_bytes != MAP_FAILED);
    memset(fake_bytes, 'x', filesize);

    char *fullpath;
    assert(asprintf(&fullpath, "%s/%s", tmp_dir, filename) > 0);

    return (struct fixture){
        strdup(tmp_dir),
        cache_dir_fd,
        .media = {
            filename,
            fullpath,
            filesize,
            .frame_rate = 42.069,
            fake_media_fd,
            fake_bytes
        }
    };
}

static void 
destroy_fixture(const struct fixture *fx)
{
    if (!fx->tmp_dir) return;

    DIR *dir = opendir(fx->tmp_dir);
    assert(dir);
    const int dir_fd = dirfd(dir);

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (strcmp(dirent->d_name, ".") == 0) continue;
        if (strcmp(dirent->d_name, "..") == 0) continue;

        unlinkat(dir_fd, dirent->d_name, 0);
    }
    closedir(dir);
    rmdir(fx->tmp_dir);

    munmap(fx->media.bytes, fx->media.size);
    close(fx->media.fd);
    close(fx->dir_fd);
    free(fx->tmp_dir);
    if (fx->media.fullpath) free(fx->media.fullpath);
}

struct test {
    const char *const name;
    void (*function)(const struct fixture *fx);
};

static void
run_test(const struct test *test)
{
    const struct fixture fx = create_fixture();

    current_test_failed = false;
    ++tests_run;

    printf("Running %s...%*s", test->name, 32-(int)strlen(test->name), "");
    (void)fflush(stdout);

    test->function(&fx);

    if (current_test_failed) {
        ++tests_failed;
    } else {
        printf("OK\n");
    }

    destroy_fixture(&fx);
}


int main(void)
{
    printf("\n");

    const struct test tests[] = {
        TEST(cache_save),
        TEST(cache_data_exists),
        TEST(cache_existence_check),
        TEST(cache_validation),
        TEST(media_mtime_changed),
        TEST(media_size_changed),
        TEST(media_path_changed),
        TEST(invalid_metadata_version),
        TEST(empty_metadata_file),
        TEST(stale_tmp_does_not_block_save),
        TEST(corrupt_metadata_file),
    };

    for (unsigned int i = 0; i < countof(tests); ++i) run_test(&tests[i]);

    printf("\n%i/%i tests passed\n\n", tests_run - tests_failed, tests_run);

    return tests_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
