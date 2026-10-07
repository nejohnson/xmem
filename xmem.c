#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *display_name;
    const char *geometry;
    const char *label;
    const char *foreground;
    const char *background;
    const char *highlight;
    const char *cache_color;
    const char *swap_color;
    int update;
    bool show_label;
    bool show_swap;
} Options;

/* All values are percentages (0-100). Used and cache are of physical memory
   and are stacked; swap is of swap space and is plotted independently. */
typedef struct {
    double used;
    double cache;
    double swap;
} Sample;

typedef struct {
    Display *display;
    Window window;
    GC foreground;
    GC background;
    GC highlight;
    GC cache;
    GC swap;
    XFontStruct *font;
    unsigned int width;
    unsigned int height;
    Sample *history;
    size_t count;
    size_t capacity;
    Sample current;
    bool has_swap;
    const Options *options;
} Graph;

static void usage(FILE *out)
{
    fprintf(out, "Usage: xmem [-update seconds] [-label text | -nolabel] [-noswap]\n"
                 "            [-display display] [-geometry geometry] [-fg color] [-bg color]\n"
                 "            [-hl color] [-cachecolor color] [-swapcolor color]\n"
                 "Plot physical memory and swap usage as percentages, updating every 5 seconds.\n"
                 "Memory in use is filled, reclaimable cache is a lighter band above it, and\n"
                 "swap is a line.\n");
}

static const char *option_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "xmem: missing value for %s\n", argv[*i]);
        exit(EXIT_FAILURE);
    }
    return argv[++*i];
}

static void parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){ .update = 5, .show_label = true, .show_swap = true,
                          .foreground = "black", .background = "white",
                          .highlight = "gray60", .cache_color = "gray80",
                          .swap_color = "red" };
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-help") || !strcmp(arg, "--help")) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(arg, "-update")) {
            const char *value = option_value(argc, argv, &i);
            char *end;
            errno = 0;
            long seconds = strtol(value, &end, 10);
            if (errno || *end || end == value || seconds < 1 || seconds > 86400) {
                fprintf(stderr, "xmem: update must be 1-86400 seconds\n");
                exit(EXIT_FAILURE);
            }
            options->update = (int)seconds;
        } else if (!strcmp(arg, "-label")) {
            options->label = option_value(argc, argv, &i);
            options->show_label = true;
        } else if (!strcmp(arg, "-nolabel")) {
            options->show_label = false;
        } else if (!strcmp(arg, "-noswap")) {
            options->show_swap = false;
        } else if (!strcmp(arg, "-display")) {
            options->display_name = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-geometry")) {
            options->geometry = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-fg") || !strcmp(arg, "-foreground")) {
            options->foreground = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-bg") || !strcmp(arg, "-background")) {
            options->background = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-hl") || !strcmp(arg, "-highlight")) {
            options->highlight = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-cachecolor")) {
            options->cache_color = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-swapcolor")) {
            options->swap_color = option_value(argc, argv, &i);
        } else {
            fprintf(stderr, "xmem: unknown option: %s\n", arg);
            usage(stderr);
            exit(EXIT_FAILURE);
        }
    }
}

static double clamp_percent(double value)
{
    if (value < 0.0) return 0.0;
    if (value > 100.0) return 100.0;
    return value;
}

/* Read /proc/meminfo (values in kB). "Used" is MemTotal - MemAvailable, so
   memory the kernel can reclaim is not counted as used. Cache is the part of
   that reclaimable memory that is page cache, buffers or reclaimable slab;
   shared memory is excluded as it cannot be dropped without swap. */
static int read_sample(Sample *sample, bool *has_swap)
{
    FILE *file = fopen("/proc/meminfo", "r");
    if (!file) return -1;
    double total = 0, available = -1, free_kb = 0, buffers = 0, cached = 0;
    double reclaimable = 0, shmem = 0, swap_total = 0, swap_free = 0;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char key[64];
        double value;
        if (sscanf(line, "%63[^:]: %lf", key, &value) != 2) continue;
        if (!strcmp(key, "MemTotal")) total = value;
        else if (!strcmp(key, "MemAvailable")) available = value;
        else if (!strcmp(key, "MemFree")) free_kb = value;
        else if (!strcmp(key, "Buffers")) buffers = value;
        else if (!strcmp(key, "Cached")) cached = value;
        else if (!strcmp(key, "SReclaimable")) reclaimable = value;
        else if (!strcmp(key, "Shmem")) shmem = value;
        else if (!strcmp(key, "SwapTotal")) swap_total = value;
        else if (!strcmp(key, "SwapFree")) swap_free = value;
    }
    fclose(file);
    if (total <= 0.0) {
        errno = ERANGE;
        return -1;
    }
    double cache = buffers + cached + reclaimable - shmem;
    if (cache < 0.0) cache = 0.0;
    if (available < 0.0) available = free_kb + cache; /* kernels before 3.14 */
    sample->used = clamp_percent(100.0 * (total - available) / total);
    sample->cache = clamp_percent(100.0 * cache / total);
    if (sample->used + sample->cache > 100.0)
        sample->cache = 100.0 - sample->used;
    *has_swap = swap_total > 0.0;
    sample->swap = *has_swap ? clamp_percent(100.0 * (swap_total - swap_free) / swap_total)
                             : 0.0;
    return 0;
}

static unsigned long color_pixel(Display *display, const char *name)
{
    XColor color, exact;
    if (!XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)),
                          name, &color, &exact)) {
        fprintf(stderr, "xmem: invalid color: %s\n", name);
        exit(EXIT_FAILURE);
    }
    return color.pixel;
}

static GC make_gc(Display *display, Window window, unsigned long pixel)
{
    XGCValues values = { .foreground = pixel };
    return XCreateGC(display, window, GCForeground, &values);
}

static void resize_history(Graph *graph, unsigned int width)
{
    size_t capacity = width;
    if (capacity == graph->capacity) return;
    size_t keep = graph->count < capacity ? graph->count : capacity;
    if (keep)
        memmove(graph->history, graph->history + graph->count - keep,
                keep * sizeof(*graph->history));
    Sample *history = realloc(graph->history, capacity * sizeof(*history));
    if (!history && capacity) {
        fprintf(stderr, "xmem: out of memory\n");
        exit(EXIT_FAILURE);
    }
    graph->history = history;
    graph->capacity = capacity;
    graph->count = keep;
}

static void append_sample(Graph *graph, Sample sample, bool has_swap)
{
    graph->current = sample;
    graph->has_swap = has_swap;
    if (!graph->capacity) return;
    if (graph->count == graph->capacity) {
        memmove(graph->history, graph->history + 1,
                (graph->count - 1) * sizeof(*graph->history));
        --graph->count;
    }
    graph->history[graph->count++] = sample;
}

static int round_up(double value)
{
    int shown = (int)value;
    if ((double)shown < value) ++shown;
    return shown;
}

static int bar_height(double percentage, int graph_height)
{
    return (int)(percentage * graph_height / 100.0 + 0.5);
}

static void draw(Graph *graph)
{
    Display *display = graph->display;
    Window window = graph->window;
    unsigned int width = graph->width, height = graph->height;
    if (!width || !height) return;
    XFillRectangle(display, window, graph->background, 0, 0, width, height);

    int top = 0;
    if (graph->options->show_label) {
        int baseline = graph->font->ascent + 3;
        char label[256];
        int n = snprintf(label, sizeof(label), "%s %d%% (+%d%%)",
                         graph->options->label ? graph->options->label : "mem",
                         round_up(graph->current.used), round_up(graph->current.cache));
        if (graph->has_swap && graph->options->show_swap && n > 0 && (size_t)n < sizeof(label))
            snprintf(label + n, sizeof(label) - (size_t)n, " swap %d%%",
                     round_up(graph->current.swap));
        XDrawString(display, window, graph->foreground, 3, baseline,
                    label, (int)strlen(label));
        top = graph->font->ascent + graph->font->descent + 6;
    }
    int bottom = (int)height - 1;
    int graph_height = bottom - top;
    if (graph_height <= 0) return;

    for (size_t i = 0; i < graph->count; ++i) {
        int x = (int)(width - graph->count + i);
        const Sample *s = &graph->history[i];
        int used = bar_height(s->used, graph_height);
        int stacked = bar_height(s->used + s->cache, graph_height);
        if (stacked > used)
            XDrawLine(display, window, graph->cache, x,
                      bottom - stacked + 1, x, bottom - used);
        if (used > 0)
            XDrawLine(display, window, graph->foreground, x,
                      bottom - used + 1, x, bottom);
    }
    for (int mark = 25; mark <= 75; mark += 25) {
        int y = bottom - graph_height * mark / 100;
        XDrawLine(display, window, graph->highlight, 0, y, (int)width - 1, y);
    }
    if (graph->has_swap && graph->options->show_swap) {
        for (size_t i = 0; i < graph->count; ++i) {
            int x = (int)(width - graph->count + i);
            int y = bottom - bar_height(graph->history[i].swap, graph_height);
            int prev = i ? bottom - bar_height(graph->history[i - 1].swap, graph_height) : y;
            XDrawLine(display, window, graph->swap, x - (i ? 1 : 0), prev, x, y);
        }
    }
}

/* Whether (u, v) lies inside a square inset from the 0-1 unit square by
   inset on each side, with corners of the given radius. */
static bool in_rounded_square(double u, double v, double inset, double radius)
{
    double lo = inset + radius, hi = 1.0 - inset - radius;
    double dx = u < lo ? lo - u : u > hi ? u - hi : 0.0;
    double dy = v < lo ? lo - v : v > hi ? v - hi : 0.0;
    if (u < inset || u > 1.0 - inset || v < inset || v > 1.0 - inset) return false;
    return dx * dx + dy * dy <= radius * radius;
}

/* Colour of the icon at (u, v), both 0-1 with v = 0 at the top: a miniature
   of the graph in a rounded frame. Returns ARGB, transparent outside it. */
static unsigned long icon_color(double u, double v)
{
    const double margin = 0.04, radius = 0.18, frame = 0.07;
    if (!in_rounded_square(u, v, margin, radius)) return 0;
    if (!in_rounded_square(u, v, margin + frame, radius - frame)) return 0xff2b3440;
    double h = 1.0 - v;
    double used = 0.30 + 0.20 * u + 0.07 * sin(u * 11.0);
    double cache = used + 0.18;
    double swap = 0.18 + 0.12 * u;
    if (h > swap - 0.04 && h < swap + 0.04) return 0xffd83a3a;
    if (h < used) return 0xff2b3440;
    if (h < cache) return 0xffa8b4c4;
    return 0xffffffff;
}

/* Set _NET_WM_ICON so panels and task switchers can show something better
   than a generic X. Each size is supersampled 4x4 for smooth edges. */
static void set_icon(Display *display, Window window)
{
    static const int sizes[] = { 16, 24, 32, 48, 64 };
    enum { SUB = 4 };
    size_t total = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
        total += 2 + (size_t)sizes[i] * (size_t)sizes[i];
    /* Format 32 properties are passed as longs whatever their size. */
    unsigned long *data = malloc(total * sizeof(*data));
    if (!data) return;
    unsigned long *p = data;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        int n = sizes[i];
        *p++ = (unsigned long)n;
        *p++ = (unsigned long)n;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                unsigned int a = 0, r = 0, g = 0, b = 0;
                for (int sy = 0; sy < SUB; ++sy) {
                    for (int sx = 0; sx < SUB; ++sx) {
                        unsigned long c = icon_color((x + (sx + 0.5) / SUB) / n,
                                                     (y + (sy + 0.5) / SUB) / n);
                        unsigned int ca = (c >> 24) & 0xff;
                        a += ca;
                        r += ((c >> 16) & 0xff) * ca / 255;
                        g += ((c >> 8) & 0xff) * ca / 255;
                        b += (c & 0xff) * ca / 255;
                    }
                }
                /* Average premultiplied samples, then un-premultiply. */
                a /= SUB * SUB;
                if (a) {
                    r = r / (SUB * SUB) * 255 / a;
                    g = g / (SUB * SUB) * 255 / a;
                    b = b / (SUB * SUB) * 255 / a;
                }
                *p++ = (unsigned long)a << 24 | (unsigned long)(r > 255 ? 255 : r) << 16 |
                       (unsigned long)(g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
        }
    }
    Atom net_wm_icon = XInternAtom(display, "_NET_WM_ICON", False);
    XChangeProperty(display, window, net_wm_icon, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)data, (int)total);
    free(data);
}

static long long monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("xmem: clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    Options options;
    parse_options(argc, argv, &options);
    Sample sample;
    bool has_swap;
    if (read_sample(&sample, &has_swap) < 0) {
        fprintf(stderr, "xmem: /proc/meminfo: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    Display *display = XOpenDisplay(options.display_name);
    if (!display) {
        fprintf(stderr, "xmem: cannot open display %s\n",
                options.display_name ? options.display_name :
                (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return EXIT_FAILURE;
    }
    int screen = DefaultScreen(display);
    int x = 0, y = 0;
    unsigned int width = 160, height = 80;
    if (options.geometry) {
        int flags = XParseGeometry(options.geometry, &x, &y, &width, &height);
        if (!flags || ((flags & WidthValue) && !width) ||
            ((flags & HeightValue) && !height)) {
            fprintf(stderr, "xmem: invalid geometry: %s\n", options.geometry);
            XCloseDisplay(display);
            return EXIT_FAILURE;
        }
        if (flags & XNegative) x += DisplayWidth(display, screen) - (int)width;
        if (flags & YNegative) y += DisplayHeight(display, screen) - (int)height;
    }
    unsigned long fg = color_pixel(display, options.foreground);
    unsigned long bg = color_pixel(display, options.background);
    unsigned long hl = color_pixel(display, options.highlight);
    unsigned long cc = color_pixel(display, options.cache_color);
    unsigned long sc = color_pixel(display, options.swap_color);
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                         x, y, width, height, 1, fg, bg);
    XStoreName(display, window, "xmem");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    XClassHint class_hint = { .res_name = "xmem", .res_class = "Xmem" };
    XSetClassHint(display, window, &class_hint);
    set_icon(display, window);
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    Graph graph = { .display = display, .window = window, .width = width,
                    .height = height, .options = &options };
    graph.foreground = make_gc(display, window, fg);
    graph.background = make_gc(display, window, bg);
    graph.highlight = make_gc(display, window, hl);
    graph.cache = make_gc(display, window, cc);
    graph.swap = make_gc(display, window, sc);
    graph.font = XLoadQueryFont(display, "fixed");
    if (!graph.font) {
        fprintf(stderr, "xmem: cannot load the fixed font\n");
        return EXIT_FAILURE;
    }
    XSetFont(display, graph.foreground, graph.font->fid);
    resize_history(&graph, width);
    append_sample(&graph, sample, has_swap);
    XMapWindow(display, window);

    long long next_sample = monotonic_ms() + (long long)options.update * 1000;
    for (;;) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == DestroyNotify) goto done;
            if (event.type == ClientMessage &&
                (Atom)event.xclient.data.l[0] == wm_delete) goto done;
            if (event.type == ConfigureNotify) {
                graph.width = (unsigned int)event.xconfigure.width;
                graph.height = (unsigned int)event.xconfigure.height;
                resize_history(&graph, graph.width);
                draw(&graph);
            } else if (event.type == Expose && event.xexpose.count == 0) {
                draw(&graph);
            }
        }
        long long now = monotonic_ms();
        if (now >= next_sample) {
            if (read_sample(&sample, &has_swap) == 0) {
                append_sample(&graph, sample, has_swap);
                draw(&graph);
            } else {
                fprintf(stderr, "xmem: /proc/meminfo: %s\n", strerror(errno));
            }
            next_sample = now + (long long)options.update * 1000;
            continue;
        }
        int timeout = (int)(next_sample - now);
        struct pollfd pfd = { .fd = ConnectionNumber(display), .events = POLLIN };
        if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) {
            perror("xmem: poll");
            break;
        }
    }
done:
    free(graph.history);
    XFreeFont(display, graph.font);
    XFreeGC(display, graph.foreground);
    XFreeGC(display, graph.background);
    XFreeGC(display, graph.highlight);
    XFreeGC(display, graph.cache);
    XFreeGC(display, graph.swap);
    XCloseDisplay(display);
    return EXIT_SUCCESS;
}
