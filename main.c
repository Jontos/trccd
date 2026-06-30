#include <errno.h>
#include <signal.h>
#include <stdcountof.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
        (void)fprintf(stderr, "libusb error: %s\n", libusb_strerror(ret));
        return nullptr;
    }

    struct libusb_device *found = nullptr;
    for (int i = 0; i < num_devices; ++i) {
        struct libusb_device_descriptor desc;
        struct libusb_device *device = list[i];

        ret = libusb_get_device_descriptor(device, &desc);
        if (ret != LIBUSB_SUCCESS) {
            (void)fprintf(stderr, "libusb error: %s\n", libusb_strerror(ret));
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
            (void)fprintf(stderr, "libusb error: %s\n", libusb_strerror(ret));
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
        (void)fprintf(stderr, "libusb error: %s\n", libusb_strerror(ret));
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
        (void)fprintf(stderr, "libusb error: %s\n", libusb_strerror(ret));
        libusb_close(device_handle);
        libusb_exit(nullptr);
        return nullptr;
    }

    return device_handle;
}

static void usb_release(libusb_device_handle **dev)
{
    const int ret = libusb_release_interface(*dev, DISPLAY_INTERFACE);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb error: %s\n", libusb_error_name(ret));
    }

    libusb_close(*dev);
    libusb_exit(nullptr);
}

static int usb_send_data(uint8_t *data, const int length, libusb_device_handle *dev)
{
    const int ret = libusb_interrupt_transfer(dev, EP_OUT, data, length, nullptr, 5000);

    if (ret < 0) {
        (void)fprintf(stderr, "libusb error: %s\n", libusb_error_name(ret));
    }

    return ret;
}

static int usb_send_header(libusb_device_handle *dev)
{
    struct [[gnu::packed]] header {
        uint32_t magic;
        uint32_t command;
        uint16_t width;
        uint16_t height;
        uint32_t unknown;
        uint32_t length;
        uint8_t padding[492];
    };

    return usb_send_data((uint8_t*)&(struct header){
        .magic = 0xDDDCDBDA,
        .command = 0x00010002,
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
        .unknown = 2,
        .length = 512,
        .padding = {}
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

static void close_process_pipe(struct process_pipe *proc_pipe)
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

static int display_video(const char *filepath, libusb_device_handle *dev)
{
    double frame_rate = MAX_FRAME_RATE;
    struct process_pipe proc_pipe = open_pipe(filepath, VIDEO, &frame_rate);
    if (!proc_pipe.stream) return -1;

    size_t frame_capacity = 300;
    uint8_t *video_buffer = malloc(frame_capacity * FRAME_SIZE);
    if (!video_buffer) {
        perror("malloc");
        close_process_pipe(&proc_pipe);
        return -1;
    }

    unsigned int num_frames = 0;
    while (keep_running) {
        const size_t nread =
            fread(&video_buffer[num_frames * FRAME_SIZE], sizeof(*video_buffer),
                  FRAME_SIZE, proc_pipe.stream);
        if (nread != FRAME_SIZE) break;

        ++num_frames;
        if (num_frames >= frame_capacity) {
            frame_capacity *= 2;
            uint8_t *new_buf = realloc(video_buffer, frame_capacity * FRAME_SIZE);
            if (!new_buf) {
                perror("realloc");
                free(video_buffer);
                close_process_pipe(&proc_pipe);
                return -1;
            }
            video_buffer = new_buf;
        }
    }
    close_process_pipe(&proc_pipe);

    if (!num_frames) {
        (void)fprintf(stderr, "Error: No frames were read from the video stream.\n");
        free(video_buffer);
        return -1;
    }

    constexpr long second_ns = 1'000'000'000L;
    const long frame_time_ns = (long)(second_ns / frame_rate);
    struct timespec start, end;

    long frame_lag_ns = 0;
    unsigned int current_frame = 0;
    while (keep_running) {
        clock_gettime(CLOCK_MONOTONIC, &start);

        uint8_t *frame_ptr = &video_buffer[current_frame * FRAME_SIZE];
        usb_send_header(dev);
        usb_send_data(frame_ptr, FRAME_SIZE, dev);
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
    free(video_buffer);

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
