/*
 * logind-test - drives src/logind.c against a fake logind (tests/fake_logind.py speaks the D-Bus on a unix socket
 * named by DBUS_SYSTEM_BUS_ADDRESS). Modes:
 *   logind-test session DEVICE   open, take DEVICE, watch it, report PauseDevice / ResumeDevice, switch VT, release
 *   logind-test fail             lm_logind_open must fail: prints "open failed: <reason>"
 *   logind-test plain DEVICE     lm_dev_open without logind is plain open(2)
 * Output is line based; the Python test reads it.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "logind.h"
#include "loop.h"

static struct lm_loop *g_loop;
static struct lm_logind *g_l;
static dev_t g_dev;
static int g_pauses, g_resumes;

static void on_pause(void *data, const char *type)
{
    (void)data;
    printf("pause %s\n", type);
    fflush(stdout);
    if (!strcmp(type, "pause"))
        lm_logind_pause_done(g_l, g_dev);
    g_pauses++;
}

static void on_resume(void *data, int fd)
{
    (void)data;
    char buf[16] = "";
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;          /* the fake puts "resumed" into the file */
    printf("resume fd=%s data=%s\n", fd >= 0 ? "ok" : "none", n > 0 ? buf : "");
    fflush(stdout);
    if (fd >= 0)
        close(fd);
    g_resumes++;
    lm_loop_quit(g_loop, 0);
}

int main(int argc, char **argv)
{
    char err[200];
    if (argc >= 3 && !strcmp(argv[1], "plain")) {
        int fd = lm_dev_open(NULL, argv[2], O_RDWR | O_NONBLOCK, NULL);
        printf("plain fd=%s\n", fd >= 0 ? "ok" : "bad");
        lm_dev_close(NULL, fd);
        return 0;
    }
    g_loop = lm_loop_create();
    g_l = lm_logind_open(g_loop, err, sizeof(err));
    if (argc >= 2 && !strcmp(argv[1], "fail")) {
        printf("open failed: %s\n", g_l ? "(it worked?)" : err);
        return g_l ? 1 : 0;
    }
    if (!g_l) {
        printf("open failed: %s\n", err);
        return 3;
    }
    struct stat st;
    if (argc < 3 || stat(argv[2], &st) < 0)
        return 2;
    g_dev = st.st_rdev;
    bool inactive = true;
    int fd = lm_dev_open(g_l, argv[2], O_RDWR | O_NONBLOCK, &inactive);
    printf("take fd=%s inactive=%d\n", fd >= 0 ? "ok" : "bad", inactive);
    if (fd >= 0 && write(fd, "hello", 5) == 5)
        printf("wrote 5\n");
    lm_logind_watch(g_l, g_dev, on_pause, on_resume, NULL);
    printf("READY\n");
    fflush(stdout);
    lm_loop_run(g_loop);
    printf("switch %d\n", lm_logind_switch_vt(g_l, 2));
    lm_dev_close(g_l, fd);
    lm_logind_close(g_l);
    lm_loop_destroy(g_loop);
    printf("DONE pauses=%d resumes=%d\n", g_pauses, g_resumes);
    return 0;
}
