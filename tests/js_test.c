/*
 * Test driver for the JavaScript interpreter (client/js*.c).
 *   js-test --selftest      run the built-in cases (also called by tests/run.sh)
 *   js-test -e 'expr'       evaluate and print the value
 *   js-test file.js         run a script (console.log goes to stdout)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js.h"

static int failures, total;

static void console_plain(void *data, const char *level, const char *msg)
{
    (void)data;
    if (strcmp(level, "log"))
        printf("[%s] ", level);
    printf("%s\n", msg);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(b);
        return NULL;
    }
    b[n] = 0;
    fclose(f);
    return b;
}

static void check(const char *src, const char *want)
{
    js *J = js_new();
    js_set_budget(J, 5000000);
    char err[300] = "";
    char *r = js_eval_string(J, src, err, sizeof(err));
    total++;
    if (!r) {
        if (strcmp(want, "!error") && strncmp(want, "!", 1)) {
            printf("FAIL: %s\n   threw: %s\n", src, err);
            failures++;
        } else if (want[0] == '!' && want[1] && !strstr(err, want + 1)) {
            printf("FAIL: %s\n   error '%s' lacks '%s'\n", src, err, want + 1);
            failures++;
        }
    } else if (strcmp(r, want)) {
        printf("FAIL: %s\n   got '%s' want '%s'\n", src, r, want);
        failures++;
    }
    free(r);
    js_free(J);
}

static void selftest(void)
{
#include "js_cases.h"
}

static const char *PRELUDE =
    "var __n = 0, __fails = 0;\n"
    "function eq(a, b, label) { __n++; var sa = JSON.stringify(a), sb = JSON.stringify(b);\n"
    "  if (sa !== sb) { __fails++; console.log('FAIL ' + label + ': got ' + sa + ' want ' + sb); } }\n"
    "function throws(fn, text, label) { __n++; try { fn(); __fails++; console.log('FAIL ' + label + ': no exception'); }\n"
    "  catch (e) { if (text && String(e.message).indexOf(text) < 0) { __fails++; console.log('FAIL ' + label + ': message ' + e.message); } } }\n";

/* module loader for the tests: specifiers are paths relative to the importing file */
static bool file_loader(void *data, const char *spec, const char *referrer, char **url, char **source, char *err, size_t errn)
{
    (void)data;
    char path[1024];
    if (spec[0] == '/' || !referrer || !referrer[0]) {
        snprintf(path, sizeof(path), "%s", spec);
    } else {
        const char *slash = strrchr(referrer, '/');
        while (!strncmp(spec, "./", 2))
            spec += 2;
        snprintf(path, sizeof(path), "%.*s%s", slash ? (int)(slash - referrer + 1) : 0, referrer, spec);
    }
    char *src = read_file(path);
    if (!src) {
        snprintf(err, errn, "cannot read %s", path);
        return false;
    }
    *url = strdup(path);
    *source = src;
    return true;
}

/* --check FILE: run a test script with eq()/throws() helpers; exit 1 if any check failed */
static int check_file(const char *path, bool module)
{
    char *src = read_file(path);
    if (!src) {
        perror(path);
        return 2;
    }
    js *J = js_new();
    js_set_console(J, console_plain, NULL);
    js_set_budget(J, 300000000);
    js_set_module_loader(J, file_loader, NULL);
    char err[400] = "";
    if (!js_run(J, PRELUDE, strlen(PRELUDE), "prelude", err, sizeof(err))) {
        fprintf(stderr, "prelude: %s\n", err);
        return 2;
    }
    bool ok = module ? js_run_module(J, src, strlen(src), path, err, sizeof(err)) : js_run(J, src, strlen(src), path, err, sizeof(err));
    if (!ok)
        printf("FAIL %s: uncaught %s\n", path, err);
    char *r = js_eval_string(J, "__n + ' checks, ' + __fails + ' failures'", err, sizeof(err));
    printf("%s: %s\n", path, r ? r : "?");
    int fails = ok && r && strstr(r, " 0 failures") ? 0 : 1;
    free(r);
    free(src);
    js_free(J);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "--check"))
        return check_file(argv[2], strstr(argv[2], ".mjs") != NULL);
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        selftest();
        printf("js selftest: %d cases, %d failures\n", total, failures);
        return failures ? 1 : 0;
    }
    js *J = js_new();
    js_set_console(J, console_plain, NULL);
    js_set_budget(J, 200000000);
    char err[400] = "";
    if (argc > 2 && !strcmp(argv[1], "-e")) {
        char *r = js_eval_string(J, argv[2], err, sizeof(err));
        if (!r) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        printf("%s\n", r);
        free(r);
        return 0;
    }
    if (argc > 1) {
        char *src = read_file(argv[1]);
        if (!src) {
            perror(argv[1]);
            return 1;
        }
        if (!js_run(J, src, strlen(src), argv[1], err, sizeof(err))) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        free(src);
        js_free(J);
        return 0;
    }
    fprintf(stderr, "usage: js-test --selftest | -e EXPR | FILE\n");
    return 2;
}
