#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdcountof.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>

#include <libusb-1.0/libusb.h>

constexpr uint16_t VID = 0x0416;
constexpr uint16_t PID = 0x5302;
constexpr uint8_t EP_OUT = 0x02;
constexpr uint8_t DISPLAY_INTERFACE = 0;

constexpr uint16_t LCD_WIDTH = 320;
constexpr uint16_t LCD_HEIGHT = 240;
constexpr uint32_t NUM_PIXELS = LCD_HEIGHT * LCD_WIDTH;
constexpr double MAX_FRAME_RATE = 22;

constexpr size_t FRAME_SIZE = NUM_PIXELS * sizeof(uint16_t);
constexpr size_t PACKET_SIZE = 512;

static volatile sig_atomic_t keep_running = true;

static void sig_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        keep_running = false;
    }
}

static libusb_device_handle *usb_open_device()
{
    int ret;
    struct libusb_device **list;
    const ssize_t num_devices = libusb_get_device_list(nullptr, &list);
    if (num_devices < 0) {
        ret = (int)num_devices;
        (void)fprintf(stderr, "libusb failed to enumerate devices: %s\n", libusb_strerror(ret));
        return nullptr;
    }

    struct libusb_device *found = nullptr;
    for (int i = 0; i < num_devices; ++i) {
        struct libusb_device_descriptor desc;
        struct libusb_device *device = list[i];

        ret = libusb_get_device_descriptor(device, &desc);
        if (ret != LIBUSB_SUCCESS) {
            (void)fprintf(stderr, "libusb failed to get device descriptor: %s\n", libusb_strerror(ret));
            continue;
        }

        if (desc.idVendor == VID && desc.idProduct == PID) {
            found = device;
            break;
        }
    }

    libusb_device_handle *device_handle = nullptr;
    if (found) {
        ret = libusb_open(found, &device_handle);
        if (ret != LIBUSB_SUCCESS) {
            (void)fprintf(stderr, "libusb failed to open device: %s\n", libusb_strerror(ret));
        }
    } else {
        (void)fprintf(stderr, "Error: Could not find device!\n");
    }
    libusb_free_device_list(list, 1);

    return device_handle;
}

static libusb_device_handle *usb_init()
{
    int ret = libusb_init_context(nullptr, nullptr, 0);
    if (ret != LIBUSB_SUCCESS) {
        (void)fprintf(stderr, "libusb failed to initialise context: %s\n", libusb_strerror(ret));
        return nullptr;
    }

    libusb_device_handle *device_handle = usb_open_device();
    if (!device_handle) {
        libusb_exit(nullptr);
        return nullptr;
    }

    libusb_set_auto_detach_kernel_driver(device_handle, 1);

    ret = libusb_claim_interface(device_handle, DISPLAY_INTERFACE);
    if (ret != LIBUSB_SUCCESS) {
        (void)fprintf(stderr, "libusb failed to claim interface: %s\n", libusb_strerror(ret));
        libusb_close(device_handle);
        libusb_exit(nullptr);
        return nullptr;
    }

    return device_handle;
}

static void usb_release(libusb_device_handle **dev)
{
    if (!*dev) return;

    const int ret = libusb_release_interface(*dev, DISPLAY_INTERFACE);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb failed to release device: %s\n", libusb_strerror(ret));
    }

    libusb_close(*dev);
    libusb_exit(nullptr);
}

static int usb_send_data(uint8_t *data, const int length, libusb_device_handle *dev)
{
    const int ret = libusb_interrupt_transfer(dev, EP_OUT, data, length, nullptr, 5000);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb failed to send data: %s\n", libusb_strerror(ret));
    }

    return ret;
}

static int usb_send_header(libusb_device_handle *dev)
{
    union header {
        struct [[gnu::packed]] {
            uint32_t magic;
            uint32_t command;
            uint16_t width;
            uint16_t height;
            uint32_t unknown;
            uint32_t length;
        };
        uint8_t padding[PACKET_SIZE];
    };

    return usb_send_data((uint8_t*)&(union header){
        .magic = 0xDDDCDBDA,
        .command = 0x00010002,
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
        .unknown = 2,
        .length = PACKET_SIZE
    }, PACKET_SIZE, dev);
}

static int keep_alive(libusb_device_handle *dev)
{
    while (keep_running) {
        if (usb_send_data((uint8_t[PACKET_SIZE]){}, PACKET_SIZE, dev) < 0) return -1;
        // 2.5s is the longest interval between transfers that still keeps the
        // connection alive
        nanosleep(&(struct timespec){
            .tv_nsec = 500000000,
            .tv_sec = 2
        }, nullptr);
    }

    return 0;
}

static int print_colour(const char *colour_str, libusb_device_handle *dev)
{
    errno = 0;
    char *endptr;
    const unsigned long colour = strtoul(colour_str, &endptr, 16);
    if (endptr == colour_str || *endptr != '\0') {
        (void)fprintf(stderr, "Failed to parse colour\n");
        return -1;
    }
    if (errno == ERANGE || colour > UINT16_MAX) {
        (void)fprintf(stderr, "Colour value out of range!\n");
        return -1;
    }

    uint8_t buffer[FRAME_SIZE];
    uint16_t *buf_ptr = (uint16_t*)buffer;
    size_t count = FRAME_SIZE / sizeof(uint16_t);
    while (count--) {
        *buf_ptr++ = (uint16_t)colour;
    }

    if (usb_send_header(dev) < 0 ||
        usb_send_data(buffer, FRAME_SIZE, dev) < 0) return -1;

    return keep_alive(dev);
}

struct process_pipe {
    FILE *stream;
    pid_t child_pid;
};

static struct process_pipe fork_child_proc(const char *args[])
{
    struct process_pipe proc_pipe = { .stream = nullptr, .child_pid = -1 };

    int fildes[2];
    if (pipe(fildes) < 0) {
        perror("Failed to open pipe");
        return proc_pipe;
    }

    enum {
        READ_END,
        WRITE_END
    };

    const pid_t pid = fork();
    switch (pid) {
        case -1:
            (void)fprintf(stderr, "Failed to fork %s: %s\n", args[READ_END],
                          strerror(errno));
            close(fildes[READ_END]);
            close(fildes[WRITE_END]);
            break;
        case 0:
            close(fildes[READ_END]);
            dup2(fildes[WRITE_END], STDOUT_FILENO);
            close(fildes[WRITE_END]);
            execvp(args[READ_END], (char *const *)args);
            (void)fprintf(stderr, "Failed to exec %s: %s\n", args[READ_END],
                          strerror(errno));
            _exit(EXIT_FAILURE);
        default:
            close(fildes[WRITE_END]);
            proc_pipe.stream = fdopen(fildes[READ_END], "r");
            if (!proc_pipe.stream) {
                perror("Failed to open pipe for reading");
                close(fildes[READ_END]);
                waitpid(pid, nullptr, 0);
                break;
            }
            proc_pipe.child_pid = pid;
    }

    return proc_pipe;
}

static void close_process_pipe(struct process_pipe *const proc_pipe)
{
    if (proc_pipe->stream && proc_pipe->stream != stdin) {
        if (fclose(proc_pipe->stream) == EOF) {
            perror("Failed to close pipe");
        }
        proc_pipe->stream = nullptr;
    }

    if (proc_pipe->child_pid > 0) {
        waitpid(proc_pipe->child_pid, nullptr, 0);
        proc_pipe->child_pid = -1;
    }
}

static long str_to_l(const char *nptr, char **endptr)
{
    errno = 0;
    const long num = strtol(nptr, endptr, 10);
    if (nptr == *endptr) {
        return -1;
    }
    if (errno == ERANGE) {
        perror("strtol");
        return -1;
    }

    return num;
}

static double probe_frame_rate(const char *filepath)
{
    if (!filepath) return MAX_FRAME_RATE;

    const char *args[] = {
        "ffprobe", 
        "-loglevel", "error",
        "-select_streams", "v:0",
        "-show_entries", "stream=avg_frame_rate",
        "-output_format", "csv=print_section=0",
        filepath, nullptr
    };

    struct process_pipe proc_pipe = fork_child_proc(args);
    if (!proc_pipe.stream) return MAX_FRAME_RATE;
    char buffer[64] = {};
    const size_t nread = fread(buffer, sizeof(*buffer), sizeof(buffer) - 1, proc_pipe.stream);
    close_process_pipe(&proc_pipe);

    if (nread == 0) return MAX_FRAME_RATE;

    char *endptr;
    const long num = str_to_l(buffer, &endptr);
    if (num < 0) {
        (void)fprintf(stderr, "ffprobe returned invalid FPS value\n");
        return MAX_FRAME_RATE;
    }

    if (*endptr != '/') {
        return (double)num < MAX_FRAME_RATE ? (double)num : MAX_FRAME_RATE;
    }

    char *nptr = endptr + 1;
    const long denom = str_to_l(nptr, &endptr);
    if (denom <= 0) {
        (void)fprintf(stderr, "ffprobe returned invalid FPS value\n");
        return MAX_FRAME_RATE;
    }

    const double frame_rate = (double)num / (double)denom;

    return frame_rate < MAX_FRAME_RATE ? frame_rate : MAX_FRAME_RATE;
}

enum mode {
    NOTHING,
    PRINT_USAGE,
    COLOUR,
    IMAGE,
    VIDEO
};

static struct process_pipe spawn_ffmpeg(const char *filepath, const enum mode mode, double *frame_rate)
{
    const struct process_pipe proc_pipe = { .stream = nullptr, .child_pid = -1 };

    char vf_str[256];
    char fps_str[32] = {};

    if (mode == VIDEO) {
        *frame_rate = probe_frame_rate(filepath);
        if (snprintf(fps_str, sizeof(fps_str), ",fps=%g", *frame_rate) < 0) {
            perror("snprintf");
            return proc_pipe;
        }
    }

    if (snprintf(vf_str, sizeof(vf_str),
                 "scale=%d:%d:force_original_aspect_ratio=increase,"
                 "crop=%d:%d,transpose=1%s",
                 LCD_WIDTH, LCD_HEIGHT,
                 LCD_WIDTH, LCD_HEIGHT,
                 fps_str) < 0)
    {
        perror("snprintf");
        return proc_pipe;
    }

    const char *args[32];
    int idx = 0;

    args[idx++] = "ffmpeg";
    args[idx++] = "-nostdin";
    args[idx++] = "-loglevel";
    args[idx++] = "fatal";
    args[idx++] = "-i";
    args[idx++] = filepath;

    if (mode == VIDEO) {
        args[idx++] = "-an";
        args[idx++] = "-sn";
    } else {
        args[idx++] = "-vframes";
        args[idx++] = "1";
    }

    args[idx++] = "-vf";
    args[idx++] = vf_str;
    args[idx++] = "-f";
    args[idx++] = "rawvideo";
    args[idx++] = "-pix_fmt";
    args[idx++] = "rgb565le";
    args[idx++] = "-";
    args[idx++] = nullptr;

    return fork_child_proc(args);
}

static struct process_pipe open_pipe(const char *path_arg, const enum mode mode, double *frame_rate)
{
    struct process_pipe proc_pipe = { .stream = nullptr, .child_pid = -1 };

    if (path_arg) {
        proc_pipe = spawn_ffmpeg(path_arg, mode, frame_rate);
        if (!proc_pipe.stream) {
            (void)fprintf(stderr, "Failed to spawn ffmpeg pipe\n");
        }
    } else {
        proc_pipe.stream = stdin;
    }

    return proc_pipe;
}

static int display_image(const char *image, libusb_device_handle *dev)
{
    struct process_pipe proc_pipe = open_pipe(image, IMAGE, nullptr);
    if (!proc_pipe.stream) return -1;

    uint8_t frame_buffer[FRAME_SIZE];
    if (fread(frame_buffer, sizeof(*frame_buffer), sizeof(frame_buffer),
              proc_pipe.stream) != sizeof(frame_buffer)) {
        if (feof(proc_pipe.stream)) {
            (void)fprintf(stderr, "Failed to read image\n");
        }
        if (ferror(proc_pipe.stream)) {
            perror("Failed to read image");
        }
        close_process_pipe(&proc_pipe);
        return -1;
    }
    close_process_pipe(&proc_pipe);

    usb_send_header(dev);
    usb_send_data(frame_buffer, FRAME_SIZE, dev);

    return keep_alive(dev);
}

static char *generate_cache_file_template()
{
    char *template = nullptr;
    const char *cache_dir = getenv("CACHE_DIRECTORY");
    if (cache_dir) {
        if (cache_dir[0] == '\0') {
            (void)fprintf (stderr, "No cache directory set, does service "
                           "file contain `CacheDirectory=`?\n");
            return nullptr;
        }
        if (asprintf(&template, "%s/XXXXXX", cache_dir) < 0) goto err;
    } else {
        cache_dir = getenv("XDG_CACHE_HOME");
        if (!cache_dir || cache_dir[0] == '\0') {
            const char *home_dir = getenv("HOME");
            if (!home_dir || home_dir[0] == '\0') {
                (void)fprintf (stderr,
                               "Can't locate cache directory, "
                               "XDG_CACHE_HOME is empty or unset\n");
                return nullptr;
            }
            if (asprintf(&template, "%s/.cache/XXXXXX", home_dir) < 0) goto err;
        } else {
            if (asprintf(&template, "%s/XXXXXX", cache_dir) < 0) goto err;
        }
    }

    return template;
err:
    perror("Failed to allocate string for cache file!");

    return nullptr;
}

// Cap video length to 24 hours
constexpr unsigned int ONE_DAY_S = 24 * 60 * 60;
constexpr size_t VIRT_MEM = FRAME_SIZE * (size_t)MAX_FRAME_RATE * ONE_DAY_S;

static void *map_temp_file(int *const fd)
{
    char *template = generate_cache_file_template();
    if (!template) return nullptr;

    *fd = mkstemp(template);
    if (*fd < 0) {
        perror("mkstemp");
        free(template);
        return nullptr;
    }
    unlink(template);
    free(template);

    void *mapped_mem = mmap(nullptr, VIRT_MEM,
                            PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped_mem == MAP_FAILED) {
        perror("Failed to create memory mapping!");
        close(*fd);
        return nullptr;
    }

    return mapped_mem;
}

struct thread_context {
    uint8_t *mem_buf;
    unsigned int frames_written;
    bool finished_reading;

    pthread_mutex_t mutex;
    pthread_cond_t frames_ready;
};

struct reader_data {
    const int mem_fd;
    struct process_pipe *const proc_pipe;

    struct thread_context *const thread_ctx;
};

static int grow_mapping(size_t *const capacity, const int fd,
                        const size_t chunk_size, void *const mem_buf)
{
    if (*capacity + chunk_size > VIRT_MEM) {
        (void)fprintf(stderr, "Memory limit exceeded, truncating video\n");
        return 1;
    }

    if (ftruncate(fd, (off_t)(*capacity + chunk_size)) < 0) {
        perror("Failed to increase size of temp file!");
        return -1;
    }

    if (mmap(mem_buf + *capacity, chunk_size,
             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, 
             fd, (off_t)*capacity) == MAP_FAILED)
    {
        perror("Failed to grow mapped memory!");
        return -1;
    }

    *capacity += chunk_size;

    return 0;
}

static void *read_frames(void *userdata)
{
    const struct reader_data *const data = userdata;
    struct thread_context *const ctx = data->thread_ctx;

    const int pipe_fd = fileno(data->proc_pipe->stream);

    constexpr size_t CHUNK_SIZE = 256 * 1024 * 1024;
    size_t file_capacity = 0;
    size_t total_mem = 0;

    void *ret = (void*)(intptr_t)-1; // NOLINT(performance-no-int-to-ptr)
    unsigned int num_frames = 0;
    while (keep_running) {
        if (total_mem == file_capacity) {
            const int mem_ret = grow_mapping(&file_capacity, data->mem_fd, CHUNK_SIZE, ctx->mem_buf);
            if (mem_ret < 0) {
                goto exit;
            } else if (mem_ret == 1) {
                break;
            }
        }

        const size_t space_left = file_capacity - total_mem;
        const ssize_t nread = splice(pipe_fd, nullptr, data->mem_fd, nullptr, space_left, SPLICE_F_MOVE);
        if (nread < 0) {
            perror("Failed to read data from ffmpeg!");
            goto exit;
        }

        total_mem += (size_t)nread;
        num_frames = (unsigned int)(total_mem / FRAME_SIZE);

        if (nread == 0) break;

        if (num_frames > ctx->frames_written) {
            pthread_mutex_lock(&ctx->mutex);
            ctx->frames_written = num_frames;
            pthread_cond_signal(&ctx->frames_ready);
            pthread_mutex_unlock(&ctx->mutex);
        }
    }

    if (num_frames) {
        ret = nullptr;
    } else {
        goto exit;
    }

    if (ftruncate(data->mem_fd, (off_t)total_mem) < 0) {
        perror("Failed to truncate temp file!");
    }
    if (total_mem < VIRT_MEM) {
        const size_t pagesize = (size_t)sysconf(_SC_PAGESIZE);
        total_mem = (total_mem + pagesize - 1) & ~(pagesize - 1);
        if (munmap(ctx->mem_buf + total_mem, VIRT_MEM - total_mem) < 0) {
            perror("Failed to unmap excess overlay memory!");
        }
    }

exit:
    close_process_pipe(data->proc_pipe);

    pthread_mutex_lock(&ctx->mutex);
    ctx->finished_reading = true;
    pthread_cond_signal(&ctx->frames_ready);
    pthread_mutex_unlock(&ctx->mutex);

    return ret;
}

struct displayer_data {
    libusb_device_handle *dev;
    const double frame_rate;

    struct thread_context *const thread_ctx;
};

static void *display_frames(void *userdata)
{
    const struct displayer_data *const data = userdata;
    struct thread_context *const ctx = data->thread_ctx;

    constexpr long second_ns = 1'000'000'000L;
    const long frame_time_ns = (long)(second_ns / data->frame_rate);
    struct timespec start, end;
    constexpr unsigned int NUM_RETRIES = 10;

    void *err = (void*)(intptr_t)-1; // NOLINT(performance-no-int-to-ptr)
    long frame_lag_ns = 0;
    unsigned int current_frame = 0;
    unsigned int num_frames = 0;
    while (keep_running) {
        pthread_mutex_lock(&ctx->mutex);

        while (!ctx->finished_reading && current_frame == ctx->frames_written &&
               keep_running) {
            pthread_cond_wait(&ctx->frames_ready, &ctx->mutex);
        }

        if (ctx->finished_reading && !ctx->frames_written) {
            (void)fprintf(stderr, "No frames to read!\n");
            pthread_mutex_unlock(&ctx->mutex);
            return (void*)(intptr_t)-1; // NOLINT(performance-no-int-to-ptr)
        }
        num_frames = ctx->frames_written;

        pthread_mutex_unlock(&ctx->mutex);

        unsigned int retries = NUM_RETRIES;
        clock_gettime(CLOCK_MONOTONIC, &start);

        while (usb_send_header(data->dev) < 0 && keep_running) {
            if (--retries == 0) {
                (void)fprintf(stderr, "USB connection timed out!\n");
                return err;
            }
        }
        retries = NUM_RETRIES;

        uint8_t *frame_ptr = &ctx->mem_buf[current_frame * FRAME_SIZE];
        while (usb_send_data(frame_ptr, FRAME_SIZE, data->dev) < 0 && keep_running) {
            if (--retries == 0) {
                (void)fprintf(stderr, "USB connection timed out!\n");
                return err;
            }
        }
        current_frame = (current_frame + 1) % num_frames;

        clock_gettime(CLOCK_MONOTONIC, &end);

        const long elapsed_ns = (end.tv_sec - start.tv_sec) * second_ns + (end.tv_nsec - start.tv_nsec);
        const long remaining_ns = frame_time_ns - elapsed_ns - frame_lag_ns;
        if (remaining_ns > 0) {
            nanosleep(&(struct timespec){
                .tv_sec = remaining_ns / second_ns,
                .tv_nsec = remaining_ns % second_ns
            }, nullptr);
            frame_lag_ns = 0;
        } else {
            frame_lag_ns = -remaining_ns;
            current_frame = (unsigned int)((current_frame + frame_lag_ns / frame_time_ns) % num_frames);
            frame_lag_ns %= frame_time_ns;
        }
    }

    return nullptr;
}

static int display_video(const char *filepath, libusb_device_handle *dev)
{
    double frame_rate = MAX_FRAME_RATE;
    struct process_pipe proc_pipe = open_pipe(filepath, VIDEO, &frame_rate);
    if (!proc_pipe.stream) return -1;

    int mem_fd;
    struct thread_context thread_ctx = {
        .mem_buf = map_temp_file(&mem_fd)
    };
    if (!thread_ctx.mem_buf) return -1;

    pthread_mutex_init(&thread_ctx.mutex, nullptr);
    pthread_cond_init(&thread_ctx.frames_ready, nullptr);

    bool prod_created = false;
    bool cons_created = false;
    pthread_t prod_tid, cons_tid;

    int ret = pthread_create(&prod_tid, nullptr, read_frames, &(struct reader_data){
        .mem_fd = mem_fd,
        .proc_pipe = &proc_pipe,
        .thread_ctx = &thread_ctx
    });
    if (ret != 0) goto cleanup;
    prod_created = true;

    ret = pthread_create(&cons_tid, nullptr, display_frames, &(struct displayer_data){
        .dev = dev,
        .frame_rate = frame_rate,
        .thread_ctx = &thread_ctx
    });
    if (ret != 0) goto cleanup;
    cons_created = true;

cleanup:
    if (ret != 0) {
        (void)fprintf(stderr, "Failed to create thread: %s\n", strerror(ret));
        keep_running = false;
    }

    int prod_ret = 0;
    int cons_ret = 0;
    void *res;
    if (prod_created) {
        pthread_join(prod_tid, &res);
        prod_ret = (int)(intptr_t)res;
        if (cons_created) {
            pthread_join(cons_tid, &res);
            cons_ret = (int)(intptr_t)res;
        }
    } else {
        close_process_pipe(&proc_pipe);
    }

    pthread_cond_destroy(&thread_ctx.frames_ready);
    pthread_mutex_destroy(&thread_ctx.mutex);

    if (ret || prod_ret || cons_ret) return -1;

    return 0;
}

static void print_usage(FILE *stream, const char *argv0)
{
    (void)fprintf(
        stream, 
        "Usage:\n"
        "\t%s <mode> [arg]\n"
        "Stream media to Thermalright LCD display.\n\n"
        "Modes:\n"
        "\tvideo\tStream video bytes from [arg] to screen. Will read from stdin if no [arg] supplied.\n"
        "\timage\tDisplay static image from [arg] on screen. Will read from stdin if no [arg] supplied.\n"
        "\tcolour\tDisplay solid colour on screen, [arg] must be a RGB565 colour in hex format.\n"
        "\thelp\tDisplay this message.\n",
        argv0
    );
}

int main(const int argc, const char **argv)
{
    struct sigaction signal = {
        .sa_handler = sig_handler,
        .sa_flags = SA_RESTART
    };
    sigemptyset(&signal.sa_mask);

    if (sigaction(SIGINT, &signal, nullptr) == -1 ||
        sigaction(SIGTERM, &signal, nullptr) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    const struct commands {
        const char *const command;
        const enum mode mode;
    } commands[] = {
        {"image", IMAGE},
        {"colour", COLOUR},
        {"color", COLOUR},
        {"video", VIDEO},
        {"help", PRINT_USAGE}
    };

    enum mode do_what = NOTHING;

    if (argc > 1) {
        for (unsigned int i = 0; i < countof(commands); ++i) {
            if (strcmp(argv[1], commands[i].command) == 0) {
                do_what = commands[i].mode;
                break;
            }
        }
    }

    [[gnu::cleanup(usb_release)]]
    libusb_device_handle *device = nullptr;
    if (do_what != NOTHING && do_what != PRINT_USAGE) {
        device = usb_init();
        if (!device) return EXIT_FAILURE;
    }

    switch (do_what) {
        case IMAGE:
            if (display_image(argc >= 3 ? argv[2] : nullptr, device) < 0) return EXIT_FAILURE;
            return EXIT_SUCCESS;
        case COLOUR:
            if (argc < 3) {
                print_usage(stderr, argv[0]);
                return EXIT_FAILURE;
            }
            if (print_colour(argv[2], device) < 0) return EXIT_FAILURE;
            return EXIT_SUCCESS;
        case VIDEO:
            if (display_video(argc >= 3 ? argv[2] : nullptr, device) < 0) return EXIT_FAILURE;
            return EXIT_SUCCESS;
        case NOTHING:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        case PRINT_USAGE:
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
    }
}
