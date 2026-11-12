#include "tuya.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "aes.h"
#include "json.h"
#include "md5.h"
#include "sha256.h"
#include "util.h"

/* The client id and schema of the device sharing API (the ones Tuya gave Home Assistant's integration; the QR login knows no others). */
#define CLIENT_ID "HA_3y9q4ak7g4ephrvke"
#define SCHEMA "haauthorize"
#define LOGIN_SERVER "https://apigw.iotbing.com"

#define HTTP_TIMEOUT_MS 25000

static void copy(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- a growing text ------------------------------------------------------------------------------------------------------------------- */

struct sbuf {
    char *s;
    size_t len, cap;
    bool bad;
};

static void sb_add(struct sbuf *b, const char *data, size_t n)
{
    if (b->bad)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2 + 256;
        char *s = realloc(b->s, cap);
        if (!s) {
            b->bad = true;
            return;
        }
        b->s = s;
        b->cap = cap;
    }
    memcpy(b->s + b->len, data, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_puts(struct sbuf *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

/* `s` as a quoted value of a curl config line. */
static void sb_curl_quoted(struct sbuf *b, const char *s)
{
    sb_puts(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            sb_add(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            sb_add(b, s, 1);
    }
    sb_puts(b, "\"\n");
}

/* ---- base64, random text ---------------------------------------------------------------------------------------------------------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(struct sbuf *b, const uint8_t *in, size_t n)
{
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        char q[4] = { B64[v >> 18], B64[(v >> 12) & 63], i + 1 < n ? B64[(v >> 6) & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
        sb_add(b, q, 4);
    }
}

/* Decodes into out (at least strlen(in) * 3 / 4 bytes); the number of bytes, or -1 for something that is not base64. */
static long b64_decode(const char *in, uint8_t *out)
{
    uint32_t acc = 0;
    int bits = 0;
    long n = 0;
    for (; *in; in++) {
        if (*in == '=' || *in == '\n' || *in == '\r')
            continue;
        const char *p = strchr(B64, *in);
        if (!p)
            return -1;
        acc = acc << 6 | (uint32_t)(p - B64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static void random_bytes(uint8_t *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = getrandom(out + got, n - got, 0);
        if (r <= 0) {
            for (; got < n; got++)                               /* (no kernel support: the ids need not be secret) */
                out[got] = (uint8_t)(rand() ^ (now_ms() >> (got % 8)));
            return;
        }
        got += (size_t)r;
    }
}

static void make_uuid(char out[37])
{
    uint8_t r[16];
    random_bytes(r, 16);
    r[6] = (r[6] & 0x0f) | 0x40;
    r[8] = (r[8] & 0x3f) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8],
             r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

/* ---- the request's secrets ---------------------------------------------------------------------------------------------------------------- */

/* hash_key = the hex MD5 of the request id + the refresh token; secret = the first 16 hex characters of HMAC-SHA256(key = request id, hash_key). */
static void make_keys(const char *rid, const char *refresh, char hash_key[33], char secret[17])
{
    char text[400];
    uint8_t md[LM_MD5_LEN], mac[LM_SHA256_LEN];
    char hex[2 * LM_SHA256_LEN + 1];
    int n = snprintf(text, sizeof(text), "%s%s", rid, refresh);
    lm_md5(text, (size_t)LM_MIN(n, (int)sizeof(text) - 1), md);
    lm_hex_encode(md, LM_MD5_LEN, hash_key);
    lm_hmac_sha256(rid, strlen(rid), hash_key, 32, mac);
    lm_hex_encode(mac, LM_SHA256_LEN, hex);
    memcpy(secret, hex, 16);
    secret[16] = 0;
}

/* base64(nonce) + base64(ciphertext + tag), the nonce being 12 characters of the API's alphabet. */
static void encrypt_text(struct sbuf *out, const char *plain, const char secret[17])
{
    static const char alphabet[] = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";
    uint8_t nonce[12], rnd[12];
    random_bytes(rnd, 12);
    for (int i = 0; i < 12; i++)
        nonce[i] = (uint8_t)alphabet[rnd[i] % (sizeof(alphabet) - 1)];
    size_t len = strlen(plain);
    uint8_t *ct = malloc(len + 16);
    if (!ct) {
        out->bad = true;
        return;
    }
    lm_aes128_gcm_encrypt((const uint8_t *)secret, nonce, NULL, 0, (const uint8_t *)plain, len, ct, ct + len);
    b64_encode(out, nonce, 12);
    b64_encode(out, ct, len + 16);
    free(ct);
}

/* The answer's `result`: base64 of nonce (12) + ciphertext + tag (16). A new text, or NULL. */
static char *decrypt_text(const char *b64, const char secret[17])
{
    uint8_t *raw = malloc(strlen(b64) + 4);
    if (!raw)
        return NULL;
    long n = b64_decode(b64, raw);
    char *out = NULL;
    if (n >= 28 && (out = malloc((size_t)n - 28 + 1))) {
        if (lm_aes128_gcm_decrypt((const uint8_t *)secret, raw, NULL, 0, raw + 12, (size_t)n - 28, raw + n - 16, (uint8_t *)out)) {
            out[n - 28] = 0;
        } else {
            free(out);
            out = NULL;
        }
    }
    free(raw);
    return out;
}

static void sign_request(const char *hash_key, const char *rid, const char *t, const char *token, const char *query_enc, const char *body_enc,
                         char out[2 * LM_SHA256_LEN + 1])
{
    struct sbuf b = { 0 };
    uint8_t mac[LM_SHA256_LEN];
    sb_puts(&b, "X-appKey=" CLIENT_ID "||X-requestId=");
    sb_puts(&b, rid);
    sb_puts(&b, "||X-time=");                                   /* (X-sid is empty and so left out) */
    sb_puts(&b, t);
    if (token && *token) {
        sb_puts(&b, "||X-token=");
        sb_puts(&b, token);
    }
    sb_puts(&b, query_enc ? query_enc : "");
    sb_puts(&b, body_enc ? body_enc : "");
    lm_hmac_sha256(hash_key, strlen(hash_key), b.s ? b.s : "", b.len, mac);
    lm_hex_encode(mac, LM_SHA256_LEN, out);
    free(b.s);
}

/* ---- curl ----------------------------------------------------------------------------------------------------------------------------- */

static const char *curl_program(void)
{
    const char *p = getenv("MELLAGUI_CURL");
    return p && *p ? p : "curl";
}

/* Runs curl with `config` on its stdin ("-K -"): the body goes to *out (the caller frees it), the HTTP status is returned; -1 with a sentence in
 * err when curl could not run, failed or took too long. */
static int run_curl(const char *config, char **out, char *err, size_t errsz)
{
    int in[2], outp[2], errp[2];
    *out = NULL;
    if (pipe2(in, O_CLOEXEC) < 0)
        goto sys;
    if (pipe2(outp, O_CLOEXEC) < 0) {
        close(in[0]); close(in[1]);
        goto sys;
    }
    if (pipe2(errp, O_CLOEXEC) < 0) {
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]);
        goto sys;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        goto sys;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        execlp(curl_program(), curl_program(), "-q", "-K", "-", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(outp[1]);
    close(errp[1]);
    size_t left = strlen(config);
    const char *p = config;
    while (left > 0) {                                          /* (a few hundred bytes: it fits the pipe) */
        ssize_t w = write(in[1], p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        p += w;
        left -= (size_t)w;
    }
    close(in[1]);
    struct sbuf body = { 0 }, etext = { 0 };
    int64_t deadline = now_ms() + HTTP_TIMEOUT_MS;
    bool timed_out = false;
    int open_fds = 2;
    struct pollfd pf[2] = { { .fd = outp[0], .events = POLLIN }, { .fd = errp[0], .events = POLLIN } };
    while (open_fds > 0) {
        int64_t wait = deadline - now_ms();
        if (wait <= 0) {
            timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
        int r = poll(pf, 2, (int)wait);
        if (r < 0 && errno != EINTR)
            break;
        for (int i = 0; i < 2; i++) {
            if (pf[i].fd < 0 || !(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char buf[8192];
            ssize_t n = read(pf[i].fd, buf, sizeof(buf));
            if (n > 0) {
                if ((i == 0 ? body.len : etext.len) < (i == 0 ? (size_t)8 << 20 : 4000))
                    sb_add(i == 0 ? &body : &etext, buf, (size_t)n);
            } else if (n == 0 || errno != EINTR) {
                pf[i].fd = -1;
                open_fds--;
            }
        }
    }
    close(outp[0]);
    close(errp[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    if (timed_out) {
        snprintf(err, errsz, "The server did not answer in time");
    } else if (code == 127) {
        snprintf(err, errsz, "The program curl is needed (sudo apt install curl)");
    } else if (code != 0) {
        char *line = etext.s ? etext.s : "";
        line[strcspn(line, "\r\n")] = 0;
        if (code == 6)
            snprintf(err, errsz, "No connection to the internet (the server's name was not found)");
        else if (code == 7 || code == 28)
            snprintf(err, errsz, "The server cannot be reached");
        else
            snprintf(err, errsz, "%s", *line ? line : "curl failed");
    } else if (body.s && body.len >= 4) {                       /* the body, a line break, the status (write-out) */
        char *nl = strrchr(body.s, '\n');
        int http = nl ? atoi(nl + 1) : 0;
        if (nl && http > 0) {
            *nl = 0;
            *out = body.s;
            free(etext.s);
            return http;
        }
        snprintf(err, errsz, "An answer that cannot be read");
    } else {
        snprintf(err, errsz, "An empty answer");
    }
    free(body.s);
    free(etext.s);
    return -1;
sys:
    snprintf(err, errsz, "curl could not be started: %s", strerror(errno));
    return -1;
}

static void config_start(struct sbuf *b, const char *method, const char *url)
{
    sb_puts(b, "silent\nshow-error\nmax-time = 20\nconnect-timeout = 8\nwrite-out = \"\\n%{http_code}\"\nrequest = ");
    sb_curl_quoted(b, method);
    sb_puts(b, "url = ");
    sb_curl_quoted(b, url);
}

static void config_header(struct sbuf *b, const char *name, const char *value)
{
    char line[700];
    if (*value)
        snprintf(line, sizeof(line), "%s: %s", name, value);
    else
        snprintf(line, sizeof(line), "%s;", name);                /* (curl's way to send a header with no value) */
    sb_puts(b, "header = ");
    sb_curl_quoted(b, line);
}

static void url_encode(struct sbuf *b, const char *s)
{
    for (; *s; s++) {
        if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            sb_add(b, s, 1);
        } else {
            char e[4];
            snprintf(e, sizeof(e), "%%%02X", (unsigned char)*s);
            sb_add(b, e, 3);
        }
    }
}

/* The answer's error as a sentence. */
static void api_error(const struct json *root, char *err, size_t errsz)
{
    const char *msg = json_str(root, "msg", "the server refused the request");
    const char *code = json_str(root, "code", "");
    snprintf(err, errsz, "%s%s%s%s", msg, *code ? " (" : "", code, *code ? ")" : "");
}

/* ---- one API call --------------------------------------------------------------------------------------------------------------------- */

static bool refresh_if_needed(struct tuya_session *s, char *err, size_t errsz);

/* Sends the request and gives the decrypted `result` as a tree the caller frees (NULL with err on a failure). params / body are JSON objects as text,
 * or NULL. *t_ms gets the server's time. */
static struct json *api_call(struct tuya_session *s, const char *method, const char *path, const char *params, const char *body, int64_t *t_ms,
                             bool refresh_first, char *err, size_t errsz)
{
    if (refresh_first && !refresh_if_needed(s, err, errsz))
        return NULL;
    char rid[37], hash_key[33], secret[17], t[24], sign[2 * LM_SHA256_LEN + 1];
    make_uuid(rid);
    make_keys(rid, s->refresh, hash_key, secret);
    struct sbuf qenc = { 0 }, benc = { 0 }, url = { 0 }, cfg = { 0 };
    if (params)
        encrypt_text(&qenc, params, secret);
    if (body)
        encrypt_text(&benc, body, secret);
    snprintf(t, sizeof(t), "%lld", (long long)now_ms());
    sign_request(hash_key, rid, t, s->access, qenc.s, benc.s, sign);
    sb_puts(&url, s->endpoint);
    sb_puts(&url, path);
    if (qenc.s) {
        sb_puts(&url, "?encdata=");
        url_encode(&url, qenc.s);
    }
    config_start(&cfg, method, url.s ? url.s : "");
    config_header(&cfg, "X-appKey", CLIENT_ID);
    config_header(&cfg, "X-requestId", rid);
    config_header(&cfg, "X-sid", "");
    config_header(&cfg, "X-time", t);
    if (s->access[0])
        config_header(&cfg, "X-token", s->access);
    config_header(&cfg, "X-sign", sign);
    if (benc.s) {
        struct sbuf data = { 0 };
        sb_puts(&data, "{\"encdata\":\"");
        sb_puts(&data, benc.s);
        sb_puts(&data, "\"}");
        config_header(&cfg, "Content-Type", "application/json");
        sb_puts(&cfg, "data-binary = ");
        sb_curl_quoted(&cfg, data.s ? data.s : "");
        free(data.s);
    }
    bool bad = qenc.bad || benc.bad || url.bad || cfg.bad || !cfg.s;
    char *text = NULL;
    int http = bad ? -1 : run_curl(cfg.s, &text, err, errsz);
    if (bad)
        snprintf(err, errsz, "Out of memory");
    free(qenc.s);
    free(benc.s);
    free(url.s);
    if (cfg.s)
        lm_secure_zero(cfg.s, cfg.len);                         /* (it held the token) */
    free(cfg.s);
    if (http < 0)
        return NULL;
    struct json *root = json_parse(text);
    free(text);
    if (!root) {
        snprintf(err, errsz, "The server answered something else than expected (HTTP %d)", http);
        return NULL;
    }
    if (!json_bool(root, "success", false)) {
        api_error(root, err, errsz);
        const char *code = json_str(root, "code", ""), *msg = json_str(root, "msg", "");
        if (!strcmp(code, "1010") || !strcmp(code, "1011") || !strcmp(code, "1004") || strcasestr(msg, "token"))
            s->relogin = true;                                  /* the tokens are not accepted any more */
        json_free(root);
        return NULL;
    }
    if (t_ms)
        *t_ms = (int64_t)json_num(root, "t", (double)now_ms());
    struct json *res = json_detach(root, "result");
    json_free(root);
    if (res && res->type == JSON_STR) {                         /* the usual case: an encrypted text */
        char *plain = decrypt_text(res->str, secret);
        if (!plain) {
            json_free(res);
            snprintf(err, errsz, "The server's answer could not be decrypted");
            return NULL;
        }
        struct json *dec = json_parse(plain);
        if (dec) {
            json_free(res);
            res = dec;
            free(plain);
        } else {                                                /* a bare text */
            free(res->str);
            res->str = plain;
        }
    }
    if (!res)
        res = json_parse("null");
    if (!res)
        snprintf(err, errsz, "Out of memory");
    return res;
}

static bool refresh_if_needed(struct tuya_session *s, char *err, size_t errsz)
{
    if (s->expire_ms - 60 * 1000 > now_ms())
        return true;
    char path[400];
    int64_t t = 0;
    snprintf(path, sizeof(path), "/v1.0/m/token/%s", s->refresh);
    struct json *r = api_call(s, "GET", path, NULL, NULL, &t, false, err, errsz);
    if (!r)
        return false;
    const char *access = json_str(r, "accessToken", ""), *refresh = json_str(r, "refreshToken", "");
    bool ok = *access && *refresh;
    if (ok) {
        copy(s->access, sizeof(s->access), access);
        copy(s->refresh, sizeof(s->refresh), refresh);
        copy(s->uid, sizeof(s->uid), json_str(r, "uid", s->uid));
        s->expire_ms = t + (int64_t)json_num(r, "expireTime", 7200) * 1000;
        tuya_session_save(s);
    } else {
        snprintf(err, errsz, "The login could not be renewed");
        s->relogin = true;
    }
    json_free(r);
    return ok;
}

/* ---- the session file ------------------------------------------------------------------------------------------------------------------- */

static bool session_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/smart.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/smart.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

bool tuya_session_load(struct tuya_session *s)
{
    char path[1024], line[700];
    memset(s, 0, sizeof(*s));
    if (!session_path(path, sizeof(path)))
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#')
            continue;
        *eq++ = 0;
        if (!strcmp(line, "user-code")) copy(s->user_code, sizeof(s->user_code), eq);
        else if (!strcmp(line, "endpoint")) copy(s->endpoint, sizeof(s->endpoint), eq);
        else if (!strcmp(line, "terminal-id")) copy(s->terminal_id, sizeof(s->terminal_id), eq);
        else if (!strcmp(line, "uid")) copy(s->uid, sizeof(s->uid), eq);
        else if (!strcmp(line, "username")) copy(s->username, sizeof(s->username), eq);
        else if (!strcmp(line, "access-token")) copy(s->access, sizeof(s->access), eq);
        else if (!strcmp(line, "refresh-token")) copy(s->refresh, sizeof(s->refresh), eq);
        else if (!strcmp(line, "expires")) s->expire_ms = strtoll(eq, NULL, 10);
    }
    fclose(f);
    return tuya_logged_in(s);
}

bool tuya_session_save(const struct tuya_session *s)
{
    char path[1024], tmp[1100];
    if (!session_path(path, sizeof(path)))
        return false;
    for (char *p = path + 1; *p; p++) {                         /* the folders above it */
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0700);
        *p = '/';
    }
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }
    fprintf(f, "# mellagui-smart: the login of the Smart Life account (tokens, no password). Written by the program itself.\n");
    fprintf(f, "user-code=%s\n", s->user_code);
    if (tuya_logged_in(s))
        fprintf(f, "endpoint=%s\nterminal-id=%s\nuid=%s\nusername=%s\naccess-token=%s\nrefresh-token=%s\nexpires=%lld\n", s->endpoint, s->terminal_id,
                s->uid, s->username, s->access, s->refresh, (long long)s->expire_ms);
    if (fclose(f) != 0 || rename(tmp, path) < 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

void tuya_session_forget(struct tuya_session *s)
{
    char code[sizeof(s->user_code)];
    copy(code, sizeof(code), s->user_code);
    lm_secure_zero(s, sizeof(*s));
    copy(s->user_code, sizeof(s->user_code), code);
    tuya_session_save(s);                                       /* (only the user code is left in the file) */
}

/* ---- the login ---------------------------------------------------------------------------------------------------------------------------- */

static const char *login_server(void)
{
    const char *e = getenv("MELLAGUI_TUYA_ENDPOINT");
    return e && *e ? e : LOGIN_SERVER;
}

static bool user_code_ok(const char *code)
{
    if (!*code || strlen(code) > 40)
        return false;
    for (; *code; code++)
        if (!isalnum((unsigned char)*code) && *code != '-' && *code != '_')
            return false;
    return true;
}

/* A request of the login (not encrypted, no token). The whole answer, or NULL. */
static struct json *login_call(const char *method, const char *url, char *err, size_t errsz)
{
    struct sbuf cfg = { 0 };
    char *text = NULL;
    config_start(&cfg, method, url);
    if (!strcmp(method, "POST"))
        sb_puts(&cfg, "data = \"\"\n");
    int http = cfg.s && !cfg.bad ? run_curl(cfg.s, &text, err, errsz) : -1;
    free(cfg.s);
    if (http < 0)
        return NULL;
    struct json *root = json_parse(text);
    free(text);
    if (!root)
        snprintf(err, errsz, "The server answered something else than expected (HTTP %d)", http);
    return root;
}

bool tuya_qr_start(const char *user_code, char *token, size_t n, char *err, size_t errsz)
{
    char url[400];
    if (!user_code_ok(user_code)) {
        snprintf(err, errsz, "That does not look like a user code (letters and digits)");
        return false;
    }
    snprintf(url, sizeof(url), "%s/v1.0/m/life/home-assistant/qrcode/tokens?clientid=%s&usercode=%s&schema=%s", login_server(), CLIENT_ID, user_code, SCHEMA);
    struct json *root = login_call("POST", url, err, errsz);
    if (!root)
        return false;
    const char *qr = json_str(json_get(root, "result"), "qrcode", "");
    bool ok = json_bool(root, "success", false) && *qr && strlen(qr) < n;
    if (ok)
        copy(token, n, qr);
    else
        api_error(root, err, errsz);
    json_free(root);
    return ok;
}

int tuya_qr_poll(const char *user_code, const char *token, struct tuya_session *s, char *err, size_t errsz)
{
    char url[700];
    struct sbuf tok = { 0 };
    if (!user_code_ok(user_code))
        return -1;
    url_encode(&tok, token);
    snprintf(url, sizeof(url), "%s/v1.0/m/life/home-assistant/qrcode/tokens/%s?clientid=%s&usercode=%s", login_server(), tok.s ? tok.s : "", CLIENT_ID, user_code);
    free(tok.s);
    struct json *root = login_call("GET", url, err, errsz);
    if (!root)
        return -1;
    const struct json *r = json_get(root, "result");
    if (!json_bool(root, "success", false) || !r || r->type != JSON_OBJ) {
        json_free(root);
        return 0;                                               /* not scanned yet */
    }
    const char *access = json_str(r, "access_token", ""), *refresh = json_str(r, "refresh_token", ""), *endpoint = json_str(r, "endpoint", "");
    int ret = -1;
    if (*access && *refresh && *endpoint) {
        memset(s, 0, sizeof(*s));
        copy(s->user_code, sizeof(s->user_code), user_code);
        copy(s->access, sizeof(s->access), access);
        copy(s->refresh, sizeof(s->refresh), refresh);
        copy(s->endpoint, sizeof(s->endpoint), endpoint);
        copy(s->terminal_id, sizeof(s->terminal_id), json_str(r, "terminal_id", ""));
        copy(s->uid, sizeof(s->uid), json_str(r, "uid", ""));
        copy(s->username, sizeof(s->username), json_str(r, "username", ""));
        s->expire_ms = (int64_t)json_num(root, "t", (double)now_ms()) + (int64_t)json_num(r, "expire_time", 7200) * 1000;
        if (!tuya_session_save(s))
            fprintf(stderr, "mellagui-smart: the login could not be saved (it lasts until the program ends)\n");
        ret = 1;
    } else {
        snprintf(err, errsz, "The login answer has no tokens");
    }
    json_free(root);
    return ret;
}

/* ---- devices ---------------------------------------------------------------------------------------------------------------------------- */

struct tuya_dp *tuya_find_dp(struct tuya_dev *d, const char *code)
{
    for (int i = 0; i < d->ndp; i++)
        if (!strcmp(d->dp[i].code, code))
            return &d->dp[i];
    return NULL;
}

static struct tuya_dp *add_dp(struct tuya_dev *d, const char *code)
{
    struct tuya_dp *dp = tuya_find_dp(d, code);
    if (dp || d->ndp >= TUYA_MAX_DP || !*code)
        return dp;
    dp = &d->dp[d->ndp++];
    memset(dp, 0, sizeof(*dp));
    copy(dp->code, sizeof(dp->code), code);
    dp->type = TUYA_OTHER;
    return dp;
}

/* A reported value into the data point; the type follows the value while the device has not said what it is. */
static void set_value(struct tuya_dp *dp, const struct json *v, bool typed)
{
    if (!v)
        return;
    dp->has = true;
    if (v->type == JSON_BOOL) {
        dp->b = v->b;
        if (!typed)
            dp->type = TUYA_BOOL;
    } else if (v->type == JSON_NUM) {
        dp->num = v->num;
        if (!typed)
            dp->type = TUYA_INT;
    } else if (v->type == JSON_STR) {
        copy(dp->str, sizeof(dp->str), v->str);
        if (dp->type == TUYA_INT && typed)
            dp->num = atof(v->str);
        if (dp->type == TUYA_BOOL && typed)
            dp->b = !strcmp(v->str, "true");
        if (!typed)
            dp->type = TUYA_STR;
    } else {
        dp->has = false;
    }
}

static void parse_device(const struct json *j, struct tuya_dev *d, const char *home)
{
    copy(d->name, sizeof(d->name), json_str(j, "name", d->id));
    copy(d->category, sizeof(d->category), json_str(j, "category", ""));
    copy(d->product, sizeof(d->product), json_str(j, "product_name", ""));
    copy(d->local_key, sizeof(d->local_key), json_str(j, "local_key", ""));
    copy(d->ip, sizeof(d->ip), json_str(j, "ip", ""));
    copy(d->home, sizeof(d->home), home);
    d->online = json_bool(j, "online", false);
    const struct json *st = json_get(j, "status");
    for (const struct json *e = st ? st->child : NULL; e; e = e->next) {
        struct tuya_dp *dp = add_dp(d, json_str(e, "code", ""));
        if (dp)
            set_value(dp, json_get(e, "value"), d->specs && dp->type != TUYA_OTHER);
    }
}

bool tuya_load_devices(struct tuya_session *s, struct tuya_dev *dev, int *n, char *err, size_t errsz)
{
    struct json *homes = api_call(s, "GET", "/v1.0/m/life/users/homes", NULL, NULL, NULL, true, err, errsz);
    if (!homes)
        return false;
    bool seen[TUYA_MAX_DEV] = { false }, ok = true;
    for (const struct json *h = homes->type == JSON_ARR ? homes->child : NULL; h && ok; h = h->next) {
        char params[120], quoted[80];
        const char *id = json_str(h, "ownerId", "");
        if (!*id)
            continue;
        snprintf(params, sizeof(params), "{\"homeId\":%s}", json_quote(id, quoted, sizeof(quoted)));
        struct json *list = api_call(s, "GET", "/v1.0/m/life/ha/home/devices", params, NULL, NULL, true, err, errsz);
        if (!list) {
            ok = false;
            break;
        }
        for (const struct json *j = list->type == JSON_ARR ? list->child : NULL; j; j = j->next) {
            const char *did = json_str(j, "id", "");
            int at = -1;
            if (!*did || strlen(did) >= sizeof(dev[0].id))
                continue;
            for (int i = 0; i < *n && at < 0; i++)
                if (!strcmp(dev[i].id, did))
                    at = i;
            if (at < 0) {
                if (*n >= TUYA_MAX_DEV)
                    continue;
                at = (*n)++;
                memset(&dev[at], 0, sizeof(dev[at]));
                copy(dev[at].id, sizeof(dev[at].id), did);
            }
            seen[at] = true;
            parse_device(j, &dev[at], json_str(h, "name", ""));
        }
        json_free(list);
    }
    json_free(homes);
    if (!ok)
        return false;
    int out = 0;
    for (int i = 0; i < *n; i++)                                /* the ones that are gone leave */
        if (seen[i]) {
            if (out != i)
                dev[out] = dev[i];
            out++;
        }
    *n = out;
    return true;
}

/* One entry of the "functions" / "status" lists: the type and the values' description (a JSON text inside the JSON). */
static void parse_spec(struct tuya_dev *d, const struct json *e, bool func)
{
    struct tuya_dp *dp = add_dp(d, json_str(e, "code", ""));
    if (!dp)
        return;
    const char *type = json_str(e, "type", "");
    enum tuya_type was = dp->type;
    dp->type = !strcmp(type, "Boolean") ? TUYA_BOOL : !strcmp(type, "Integer") ? TUYA_INT : !strcmp(type, "Enum") ? TUYA_ENUM
             : !strcmp(type, "String") ? TUYA_STR : TUYA_OTHER;
    if (func)
        dp->func = true;
    if (dp->has && was == TUYA_STR && dp->type == TUYA_INT)
        dp->num = atof(dp->str);
    const struct json *vals = json_get(e, "values");
    struct json *own = NULL;
    if (vals && vals->type == JSON_STR)
        vals = own = json_parse(vals->str);
    if (vals && vals->type == JSON_OBJ) {
        if (dp->type == TUYA_INT) {
            dp->min = json_num(vals, "min", 0);
            dp->max = json_num(vals, "max", 0);
            dp->step = json_num(vals, "step", 1);
            dp->scale = (int)json_num(vals, "scale", 0);
            if (dp->scale < 0 || dp->scale > 6)
                dp->scale = 0;
            copy(dp->unit, sizeof(dp->unit), json_str(vals, "unit", ""));
        } else if (dp->type == TUYA_ENUM) {
            const struct json *range = json_get(vals, "range");
            dp->nrange = 0;
            for (const struct json *r = range ? range->child : NULL; r && dp->nrange < TUYA_MAX_RANGE; r = r->next)
                if (r->type == JSON_STR && strlen(r->str) < sizeof(dp->range[0]))
                    copy(dp->range[dp->nrange++], sizeof(dp->range[0]), r->str);
        }
    }
    json_free(own);
}

bool tuya_load_specs(struct tuya_session *s, struct tuya_dev *d, char *err, size_t errsz)
{
    char path[160];
    d->specs = true;
    snprintf(path, sizeof(path), "/v1.1/m/life/%s/specifications", d->id);
    struct json *r = api_call(s, "GET", path, NULL, NULL, NULL, true, err, errsz);
    if (!r)
        return false;
    const struct json *list = json_get(r, "status");
    for (const struct json *e = list ? list->child : NULL; e; e = e->next)
        parse_spec(d, e, false);
    list = json_get(r, "functions");
    for (const struct json *e = list ? list->child : NULL; e; e = e->next)
        parse_spec(d, e, true);
    json_free(r);
    return true;
}

bool tuya_command(struct tuya_session *s, const char *dev_id, const char *code, const char *value_json, char *err, size_t errsz)
{
    char path[160], body[400], quoted[100];
    snprintf(path, sizeof(path), "/v1.1/m/thing/%s/commands", dev_id);
    int n = snprintf(body, sizeof(body), "{\"commands\":[{\"code\":%s,\"value\":%s}]}", json_quote(code, quoted, sizeof(quoted)), value_json);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        snprintf(err, errsz, "The value is too long");
        return false;
    }
    struct json *r = api_call(s, "POST", path, NULL, body, NULL, true, err, errsz);
    if (!r)
        return false;
    json_free(r);
    return true;
}

void tuya_dp_text(const struct tuya_dp *dp, char *out, size_t n)
{
    if (!dp->has) {
        snprintf(out, n, "-");
    } else if (dp->type == TUYA_BOOL) {
        snprintf(out, n, "%s", dp->b ? "on" : "off");
    } else if (dp->type == TUYA_INT) {
        double v = dp->num;
        for (int i = 0; i < dp->scale; i++)
            v /= 10.0;
        snprintf(out, n, "%.*f%s%s", dp->scale, v, dp->unit[0] && strcmp(dp->unit, "%") ? " " : "", dp->unit);
    } else {
        snprintf(out, n, "%s", dp->str);
    }
}

/* ---- selftest ----------------------------------------------------------------------------------------------------------------------------- */

int tuya_selftest(void)
{
    int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "mellagui-smart selftest: " __VA_ARGS__); fputc('\n', stderr); bad++; } } while (0)
    /* MD5 (RFC 1321) */
    uint8_t md[16];
    char hex[80];
    lm_md5("abc", 3, md);
    lm_hex_encode(md, 16, hex);
    CHECK(!strcmp(hex, "900150983cd24fb0d6963f7d28e17f72"), "md5 abc = %s", hex);
    lm_md5("12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80, md);
    lm_hex_encode(md, 16, hex);
    CHECK(!strcmp(hex, "57edf4a22be3c955ac49da2e2107b67a"), "md5 long = %s", hex);
    /* AES-128 (FIPS 197, appendix C.1) */
    static const uint8_t key[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t plain[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    struct lm_aes128 a;
    uint8_t blk[16], back[16];
    lm_aes128_init(&a, key);
    lm_aes128_encrypt_block(&a, plain, blk);
    lm_hex_encode(blk, 16, hex);
    CHECK(!strcmp(hex, "69c4e0d86a7b0430d8cdb78070b4c55a"), "aes = %s", hex);
    lm_aes128_decrypt_block(&a, blk, back);
    CHECK(!memcmp(back, plain, 16), "aes decrypt");
    /* AES-GCM (the GCM specification's test case 2: zero key, zero nonce, one zero block) */
    static const uint8_t zero[16] = { 0 };
    uint8_t ct[16], tag[16];
    lm_aes128_gcm_encrypt(zero, zero, NULL, 0, zero, 16, ct, tag);
    lm_hex_encode(ct, 16, hex);
    CHECK(!strcmp(hex, "0388dace60b6a392f328c2b971b2fe78"), "gcm text = %s", hex);
    lm_hex_encode(tag, 16, hex);
    CHECK(!strcmp(hex, "ab6e47d42cec13bdf53a67b21257bddf"), "gcm tag = %s", hex);
    CHECK(lm_aes128_gcm_decrypt(zero, zero, NULL, 0, ct, 16, tag, back) && !memcmp(back, zero, 16), "gcm decrypt");
    tag[0] ^= 1;
    CHECK(!lm_aes128_gcm_decrypt(zero, zero, NULL, 0, ct, 16, tag, back), "gcm accepts a wrong tag");
    /* a request's text there and back */
    char hash_key[33], secret[17];
    make_keys("0f8fad5b-d9cb-469f-a165-70867728950e", "refresh", hash_key, secret);
    CHECK(strlen(hash_key) == 32 && strlen(secret) == 16, "keys");
    struct sbuf enc = { 0 };
    encrypt_text(&enc, "{\"homeId\":\"12345\"}", secret);
    CHECK(enc.s && strlen(enc.s) > 16, "encrypt");
    if (enc.s) {
        char *p = decrypt_text(enc.s, secret);                  /* (the two base64 parts decode as one) */
        CHECK(p && !strcmp(p, "{\"homeId\":\"12345\"}"), "decrypt gives '%s'", p ? p : "(null)");
        free(p);
        enc.s[20] = enc.s[20] == 'A' ? 'B' : 'A';
        p = decrypt_text(enc.s, secret);
        CHECK(!p, "a changed text is accepted");
        free(p);
    }
    free(enc.s);
    /* JSON */
    struct json *j = json_parse(" {\"a\":[1,2.5,{\"b\":\"x\\u00e4\\n\\\"\"}],\"t\":true,\"n\":null,\"id\":12345678901234567890} ");
    CHECK(j && json_get(j, "a") && json_get(j, "a")->child->next->num == 2.5, "json numbers");
    CHECK(j && json_bool(j, "t", false) && !strcmp(json_str(j, "id", ""), "12345678901234567890"), "json members");
    CHECK(j && !strcmp(json_str(json_get(j, "a")->child->next->next, "b", ""), "x\xc3\xa4\n\""), "json string");
    json_free(j);
    CHECK(!json_parse("{\"a\":1,}") && !json_parse("[1 2]") && !json_parse("{\"a\"}") && !json_parse("\"open") && !json_parse("1 2"), "bad json is accepted");
    char q[40];
    CHECK(!strcmp(json_quote("a\"b\\c\n", q, sizeof(q)), "\"a\\\"b\\\\c\\u000a\""), "json_quote: %s", q);
    /* a device's answer */
    static struct tuya_dev d;
    memset(&d, 0, sizeof(d));
    j = json_parse("{\"id\":\"dev1\",\"name\":\"Blind\",\"category\":\"cl\",\"online\":true,\"local_key\":\"k\",\"status\":["
                   "{\"code\":\"control\",\"value\":\"stop\"},{\"code\":\"percent_control\",\"value\":40},{\"code\":\"switch\",\"value\":true}]}");
    parse_device(j, &d, "Home");
    json_free(j);
    CHECK(d.ndp == 3 && d.online && !strcmp(d.name, "Blind") && d.dp[1].num == 40 && d.dp[2].b, "device (%d)", d.ndp);
    j = json_parse("{\"code\":\"control\",\"type\":\"Enum\",\"values\":\"{\\\"range\\\":[\\\"open\\\",\\\"stop\\\",\\\"close\\\"]}\"}");
    parse_spec(&d, j, true);
    json_free(j);
    j = json_parse("{\"code\":\"percent_control\",\"type\":\"Integer\",\"values\":\"{\\\"unit\\\":\\\"%\\\",\\\"min\\\":0,\\\"max\\\":100,\\\"scale\\\":0,\\\"step\\\":1}\"}");
    parse_spec(&d, j, true);
    json_free(j);
    CHECK(d.dp[0].type == TUYA_ENUM && d.dp[0].nrange == 3 && !strcmp(d.dp[0].range[2], "close") && d.dp[0].func, "enum spec");
    CHECK(d.dp[1].type == TUYA_INT && d.dp[1].max == 100 && !strcmp(d.dp[1].unit, "%"), "integer spec");
    tuya_dp_text(&d.dp[1], q, sizeof(q));
    CHECK(!strcmp(q, "40%"), "value text '%s'", q);
    struct tuya_dp power = { .type = TUYA_INT, .has = true, .num = 1234, .scale = 1, .unit = "W" };
    tuya_dp_text(&power, q, sizeof(q));
    CHECK(!strcmp(q, "123.4 W"), "scaled text '%s'", q);
    CHECK(user_code_ok("Ab12cd") && !user_code_ok("") && !user_code_ok("a b") && !user_code_ok("a&b=1"), "user codes");
    uint8_t raw[16];
    CHECK(b64_decode("aGVsbG8=", raw) == 5 && !memcmp(raw, "hello", 5) && b64_decode("a*b", raw) < 0, "base64");
#undef CHECK
    return bad;
}
