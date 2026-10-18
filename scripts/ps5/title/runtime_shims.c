/**
 * wiliwili - PS5 native title runtime shims.
 *
 * Adapted from ps5-opengl's native-app/runtime_shims.c and
 * ps5-native-app-boilerplate's ProsperoLight component
 * (Copyright (C) 2026 BlackBearReloaded, GPL-3.0-or-later).
 *
 * A native title links one static image: there is no dynamic loader that would
 * satisfy the libc entry points the bundled libraries expect, and returning from
 * main terminates the process instead of handing control back to a shell.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int sceKernelUsleep(uint32_t microseconds);
extern uint64_t sceKernelGetProcessTime(void);

/* The console keeps its own splash picture up until the title draws something,
 * and the OpenGL runtime hides it as soon as the display opens. The screen stays
 * black until the fonts and the first page are ready; holding the splash longer
 * needs a call back into the launcher, which is not wired up yet. */

/**
 * Returning from main or calling exit() ends the process, and the console
 * reports the crash over the title. Keep the application suspended instead so
 * the log stays readable after a failure.
 */
__attribute__((noreturn)) void catchReturnFromMain(int status)
{
    printf("[wiliwili] main returned status=%d\n", status);
    fflush(NULL);
    for (;;)
        sceKernelUsleep(100000);
}

/**
 * A title's standard streams have no reader, so the whole application log is
 * moved to the directory wiliwili stores its configuration in. /download0 is the
 * title's private data area next to /app0 and is used if that directory cannot
 * be created.
 */
static FILE *wiliwili_log_open(const char *directory)
{
    if (mkdir(directory, 0755) != 0 && errno != EEXIST)
        return NULL;
    char path[256];
    if ((size_t)snprintf(path, sizeof(path), "%s/wiliwili-title.log", directory) >= sizeof(path))
        return NULL;
    FILE *stream = freopen(path, "w", stdout);
    if (stream == NULL)
        return NULL;
    setvbuf(stream, NULL, _IOLBF, 0);
    if (freopen(path, "a", stderr) != NULL)
        setvbuf(stderr, NULL, _IOLBF, 0);
    return stream;
}

__attribute__((constructor)) static void wiliwili_open_log(void)
{
    const char *directories[] = {"/data/wiliwili", "/download0", NULL};
    for (size_t i = 0; directories[i] != NULL; ++i) {
        if (wiliwili_log_open(directories[i]) != NULL)
            return;
    }
}

__attribute__((noreturn)) void __assert(const char *function, const char *file, int line,
                                        const char *expression)
{
    fprintf(stderr, "[wiliwili] assertion failed: %s (%s:%d, %s)\n", expression, file, line,
            function);
    abort();
}

/* The SDK binds these to libScePosixForWebKit, a system library a title does not
 * load: without a definition the call jumps to nothing and the application stops
 * at its first temporary file or socket pair. */
int mkstemps(char *template_name, int suffix_length)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned counter;
    const size_t length = template_name ? strlen(template_name) : 0;

    if (suffix_length < 0 || length < (size_t)suffix_length + 6u) {
        errno = EINVAL;
        return -1;
    }
    char *name = template_name + length - (size_t)suffix_length - 6u;
    for (int i = 0; i < 6; ++i) {
        if (name[i] != 'X') {
            errno = EINVAL;
            return -1;
        }
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        uint64_t value = sceKernelGetProcessTime() +
                         (uint64_t)__atomic_add_fetch(&counter, 1u, __ATOMIC_RELAXED) *
                             UINT64_C(0x9E3779B97F4A7C15);
        for (int i = 0; i < 6; ++i) {
            name[i] = letters[value % 36u];
            value /= 36u;
        }
        const int file = open(template_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (file >= 0 || errno != EEXIST)
            return file;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *template_name)
{
    return mkstemps(template_name, 0);
}

/* Bound to the same library, and nothing under a title is a terminal. */
int isatty(int descriptor)
{
    (void)descriptor;
    errno = ENOTTY;
    return 0;
}

void openlog(const char *identifier, int option, int facility)
{
    (void)identifier;
    (void)option;
    (void)facility;
}

FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}
