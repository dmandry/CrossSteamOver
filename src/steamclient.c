/* CrossSteamOver steamclient64.so
 *
 * Substantial portions of the call bridge, Steam Input struct returns, and the
 * achievement-repeat filter are adapted from VXRuntime (VeRantyx), Copyright
 * (c) 2026 Ag3497120, MIT License. https://github.com/Ag3497120/VeRantyx
 */

#define _DARWIN_C_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WINAPI __attribute__((ms_abi))
typedef uint64_t u64;
typedef int32_t BOOL;
typedef uint32_t DWORD;
typedef uint16_t WCHAR;

#define SYSV __attribute__((sysv_abi))
typedef SYSV u64 (*sysv_fn)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);
typedef struct { u64 lo, hi; } sysv_pair;
typedef SYSV sysv_pair (*sysv_pair_fn)(u64, u64, u64, u64, u64, u64, u64, u64, u64, u64);

#define METHODS 256
enum { PLAIN, RET_CLASS8, RET_INPUT_DIGITAL, RET_INPUT_ANALOG, RET_INPUT_MOTION };

typedef struct { void **vtbl; void *mac; char name[56]; uint8_t called[METHODS]; } proxy;

static void *g_lib;
static int g_trace;
static void *g_vtbl[METHODS];
static proxy *g_proxies[512];
static int g_nproxies;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log;

__attribute__((constructor))
static void bridge_ctor(void) {
    FILE *f = fopen("/tmp/steambridge.log", "a");
    if (f) { fputs("ctor\n", f); fclose(f); }
}

static void log_line(const char *fmt, ...) {
    va_list ap;
    if (!g_log) g_log = fopen("/tmp/steambridge.log", "a");
    va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fputc('\n', g_log); fflush(g_log); }
    va_end(ap);
}

static void *mac_lib(void) {
    static int tried;
    if (tried) return g_lib;
    tried = 1;
    const char *path = getenv("VXRT_STEAM_CLIENT");
    char buf[1024];
    if (!path || !path[0]) {
        const char *home = getenv("HOME");
        snprintf(buf, sizeof buf, "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", home ? home : "");
        path = buf;
    }
    struct sigaction before[32], now;
    for (int i = 1; i < 32; i++) sigaction(i, NULL, &before[i]);
    g_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    for (int i = 1; i < 32; i++) if (i != SIGKILL && i != SIGSTOP) {
        sigaction(i, NULL, &now);
        if (now.sa_sigaction != before[i].sa_sigaction) sigaction(i, &before[i], NULL);
    }
    if (!g_lib) log_line("cannot load %s: %s", path, dlerror());
    else log_line("loaded %s", path);
    return g_lib;
}

static int peek(const void *p, void *out, size_t n) {
    if ((uintptr_t)p < 0x10000) return 0;
    memcpy(out, p, n);
    return 1;
}

static const char *version_string(u64 arg, char *out, size_t cap) {
    if (!peek((const void *)arg, out, cap - 1)) return NULL;
    out[cap - 1] = 0;
    size_t len = strnlen(out, cap - 1);
    if (len < 8 || len > 60) return NULL;
    if (strncmp(out, "Steam", 5) && strncmp(out, "STEAM", 5)) return NULL;
    if (out[len - 1] < '0' || out[len - 1] > '9') return NULL;
    return out;
}

static proxy *wrap(void *mac, const char *name) {
    if (!mac) return NULL;
    pthread_mutex_lock(&g_lock);
    proxy *p = NULL;
    for (int i = 0; i < g_nproxies && !p; i++)
        if (g_proxies[i]->mac == mac && !strcmp(g_proxies[i]->name, name)) p = g_proxies[i];
    if (!p && g_nproxies < 512) {
        p = calloc(1, sizeof *p);
        p->vtbl = g_vtbl;
        p->mac = mac;
        snprintf(p->name, sizeof p->name, "%s", name ? name : "");
        g_proxies[g_nproxies++] = p;
        log_line("interface %s", p->name);
    }
    pthread_mutex_unlock(&g_lock);
    return p;
}

/* A Windows function pointer handed to the Mac client must be called with the Microsoft ABI. */
typedef WINAPI u64 (*ms_fn)(u64, u64, u64, u64);
static ms_fn g_tramp_target[8];
#define TRAMP(i) static SYSV u64 tramp##i(u64 a, u64 b, u64 c, u64 d) { \
    void *teb; __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb)); \
    if (!teb) return 0; \
    return g_tramp_target[i](a, b, c, d); }
TRAMP(0) TRAMP(1) TRAMP(2) TRAMP(3) TRAMP(4) TRAMP(5) TRAMP(6) TRAMP(7)
static void *const g_tramps[8] = { (void *)tramp0, (void *)tramp1, (void *)tramp2, (void *)tramp3,
    (void *)tramp4, (void *)tramp5, (void *)tramp6, (void *)tramp7 };

typedef struct { uint16_t len, maxlen; WCHAR *buf; } ustr;
/* A function pointer handed to the Mac client starts a .pdata entry. Strings and structs that
 * live in a Windows image must not be treated as code. */
static int in_windows_code(u64 v) {
    if (v < 0x10000) return 0;
    void *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    if (!teb) return 0;
    uint8_t *peb = *(uint8_t **)((uint8_t *)teb + 0x60);
    uint8_t *ldr = peb ? *(uint8_t **)(peb + 0x18) : NULL;
    if (!ldr) return 0;
    uint8_t *head = ldr + 0x20; /* InMemoryOrderModuleList; e points at the links */
    for (uint8_t *e = *(uint8_t **)head; e && e != head; e = *(uint8_t **)e) {
        uint8_t *base = *(uint8_t **)(e + 0x20);
        uint32_t size = *(uint32_t *)(e + 0x30);
        if (!base || v < (u64)(uintptr_t)base || v >= (u64)(uintptr_t)base + size) continue;
        if (*(uint16_t *)base != 0x5a4d) return 0;
        int32_t lfanew = *(int32_t *)(base + 0x3c);
        uint8_t *nt = base + lfanew;
        if (*(uint16_t *)(nt + 24) != 0x20b) return 0;
        uint32_t ndd = *(uint32_t *)(nt + 24 + 108);
        if (ndd <= 3) return 0;
        uint32_t pdata_rva = *(uint32_t *)(nt + 24 + 112 + 24);
        uint32_t pdata_sz = *(uint32_t *)(nt + 24 + 116 + 24);
        if (!pdata_rva || pdata_sz < 12) return 0;
        const uint32_t *fn = (const uint32_t *)(base + pdata_rva);
        uint32_t rva = (uint32_t)(v - (u64)(uintptr_t)base);
        size_t lo = 0, hi = pdata_sz / 12;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (fn[3 * mid] < rva) lo = mid + 1;
            else hi = mid;
        }
        return lo < pdata_sz / 12 && fn[3 * lo] == rva;
    }
    return 0;
}
static u64 sysv_callback(u64 fn) {
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < 8 && g_tramp_target[i] && (u64)(uintptr_t)g_tramp_target[i] != fn) i++;
    if (i < 8) g_tramp_target[i] = (ms_fn)(uintptr_t)fn;
    pthread_mutex_unlock(&g_lock);
    if (i == 8) { log_line("too many callbacks"); return 0; }
    return (u64)(uintptr_t)g_tramps[i];
}

static const struct { const char *iface; int index, kind; } g_special[] = {
    { "SteamUser0", 2, RET_CLASS8 },
    { "SteamInput006", 17, RET_INPUT_DIGITAL },
    { "SteamInput006", 21, RET_INPUT_ANALOG },
    { "SteamInput006", 29, RET_INPUT_MOTION },
    { "SteamInput007", 17, RET_INPUT_DIGITAL },
    { "SteamInput007", 21, RET_INPUT_ANALOG },
    { "SteamInput007", 29, RET_INPUT_MOTION },
};
static int special(const proxy *p, int k) {
    for (size_t i = 0; i < sizeof g_special / sizeof g_special[0]; i++)
        if (g_special[i].index == k && !strncmp(p->name, g_special[i].iface, strlen(g_special[i].iface)))
            return g_special[i].kind;
    return PLAIN;
}

/* DS2 looks up Menu and InGame before it calls SetInputActionManifestFilePath. */
static void steaminput_manifest(void *mac) {
    static int done;
    if (done) return;
    done = 1;
    void *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    if (!teb) return;
    uint8_t *peb = *(uint8_t **)((uint8_t *)teb + 0x60);
    uint8_t *params = peb ? *(uint8_t **)(peb + 0x20) : NULL;
    if (!params) return;
    ustr image;
    memcpy(&image, params + 0x60, sizeof image);
    if (!image.buf || image.len < 8) return;
    char dos[1024];
    int n = 0;
    for (int i = 0; i < image.len / 2 && n < (int)sizeof dos - 1; i++) {
        WCHAR c = image.buf[i];
        dos[n++] = c < 128 ? (char)c : '?';
    }
    dos[n] = 0;
    const char *rest = dos;
    if (!strncmp(rest, "\\??\\", 4) || !strncmp(rest, "\\\\?\\", 4)) rest += 4;
    int letter = (rest[0] >= 'A' && rest[0] <= 'Z') || (rest[0] >= 'a' && rest[0] <= 'z');
    if (!letter || rest[1] != ':') return;
    const char *prefix = getenv("WINEPREFIX");
    if (!prefix) return;
    char link[1024], root[1024], unixpath[1200], manifest[1300];
    snprintf(link, sizeof link, "%s/dosdevices/%c:", prefix, rest[0] >= 'A' && rest[0] <= 'Z' ? rest[0] - 'A' + 'a' : rest[0]);
    if (!realpath(link, root)) return;
    const char *tail = rest + 2;
    snprintf(unixpath, sizeof unixpath, "%s%s", root, tail);
    for (char *s = unixpath; *s; s++) if (*s == '\\') *s = '/';
    char *slash = strrchr(unixpath, '/');
    if (!slash) { log_line("action manifest: no folder in %s", unixpath); return; }
    snprintf(manifest, sizeof manifest, "%.*s/steaminput/steam_input_manifest.vdf", (int)(slash - unixpath), unixpath);
    log_line("action manifest candidate %s", manifest);
    if (access(manifest, R_OK) != 0) return;
    void **mvt = *(void ***)mac;
    sysv_fn set = (sysv_fn)mvt[2];
    u64 ok = set ? set((u64)(uintptr_t)mac, (u64)(uintptr_t)manifest, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
    log_line("action manifest %s -> %llu", manifest, (unsigned long long)ok);
}

/* StoreStats publishes every achievement the game reports again. Steam already has an unlock
 * time for each one earned earlier, and that is what decides. A live query can still say the
 * trophy is locked while that cache is being applied. */
static uint64_t trophy_unlocked;
static int trophy_state;
static int stat_have[64];
static int stat_value[64];
static int stats_dirty;
static int schema_trophy[128];

static int trophy_number(const char *name) {
    char buf[32];
    int i = 0;
    if (!name) return -1;
    while (i < 31) {
        char c = 0;
        if (!peek(name + i, &c, 1)) return -1;
        buf[i++] = c;
        if (!c) break;
    }
    if (!buf[0] || strncmp(buf, "trophy_", 7)) return -1;
    char *end = NULL;
    long id = strtol(buf + 7, &end, 10);
    if (end == buf + 7 || id < 0 || id > 63) return -1;
    if (*end && strcmp(end, "_stat")) return -1;
    return (int)id;
}
static int read_file(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return 0; }
    rewind(f);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return 0; }
    fclose(f);
    *out = b;
    *n = (size_t)sz;
    return 1;
}
static void read_schema_and_cache(uint8_t *b, size_t n, int schema) {
    const char *stack[8] = { 0 };
    int depth = 0, stat_id = -1;
    size_t i = 0;
    while (i < n) {
        uint8_t t = b[i++];
        if (t == 8) { if (depth) depth--; continue; }
        if (i >= n) break;
        char *key = (char *)b + i;
        while (i < n && b[i]) i++;
        if (i >= n) break;
        i++;
        if (t == 0) {
            const char *parent = depth > 0 ? stack[depth - 1] : NULL;
            if (parent && !strcmp(parent, schema ? "stats" : "cache")) stat_id = atoi(key);
            if (depth < 8) stack[depth] = key;
            depth++;
            continue;
        }
        if (t == 1) {
            char *val = (char *)b + i;
            while (i < n && b[i]) i++;
            if (i < n) i++;
            if (schema && !strcmp(key, "name") && stat_id >= 0 && stat_id < 128) {
                size_t ln = strlen(val);
                int trophy = ln >= 5 && !strcmp(val + ln - 5, "_stat") ? trophy_number(val) : -1;
                if (trophy >= 0) schema_trophy[stat_id] = trophy;
            }
            continue;
        }
        if (t == 2) {
            if (i + 4 > n) break;
            int32_t v = (int32_t)(b[i] | (b[i + 1] << 8) | (b[i + 2] << 16) | (b[i + 3] << 24));
            i += 4;
            if (!schema && !strcmp(key, "data") && stat_id >= 0 && stat_id < 128 && schema_trophy[stat_id] >= 0) {
                int trophy = schema_trophy[stat_id];
                stat_have[trophy] = 1;
                stat_value[trophy] = v;
            }
            if (!schema && depth > 0 && stack[depth - 1] && !strcmp(stack[depth - 1], "AchievementTimes")) {
                int id = atoi(key);
                if (id >= 0 && id < 64) trophy_unlocked |= (uint64_t)1 << id;
            }
            continue;
        }
        break;
    }
}
static void load_trophy_unlocked(void) {
    if (trophy_state) return;
    trophy_state = 1;
    for (int i = 0; i < 128; i++) schema_trophy[i] = -1;
    const char *home = getenv("HOME");
    const char *app = getenv("SteamAppId");
    if (!app || !app[0]) app = getenv("SteamGameId");
    if (!home || !app || !app[0]) return;
    char dirpath[1024], path[1200];
    snprintf(dirpath, sizeof dirpath, "%s/Library/Application Support/Steam/appcache/stats", home);
    snprintf(path, sizeof path, "%s/UserGameStatsSchema_%s.bin", dirpath, app);
    uint8_t *buf = NULL;
    size_t n = 0;
    if (read_file(path, &buf, &n)) { read_schema_and_cache(buf, n, 1); free(buf); }
    DIR *dir = opendir(dirpath);
    if (!dir) return;
    char suffix[64];
    snprintf(suffix, sizeof suffix, "_%s.bin", app);
    size_t suffix_n = strlen(suffix);
    struct dirent *ent;
    path[0] = 0;
    while ((ent = readdir(dir))) {
        size_t len = strlen(ent->d_name);
        if (strncmp(ent->d_name, "UserGameStats_", 14) || strstr(ent->d_name, "Schema")) continue;
        if (len < suffix_n || strcmp(ent->d_name + len - suffix_n, suffix)) continue;
        snprintf(path, sizeof path, "%s/%s", dirpath, ent->d_name);
        break;
    }
    closedir(dir);
    if (!path[0] || !read_file(path, &buf, &n)) return;
    read_schema_and_cache(buf, n, 0);
    free(buf);
    int count = 0;
    for (int i = 0; i < 64; i++) if (trophy_unlocked & ((uint64_t)1 << i)) count++;
    if (count) log_line("%d achievements already on this account", count);
}
static int stats_iface(const proxy *p) {
    return strstr(p->name, "USERSTATS") || strstr(p->name, "UserStats");
}
static int skip_repeat_trophy(proxy *p, int k, u64 *a) {
    if (!stats_iface(p) || (k != 3 && k != 4 && k != 7 && k != 10 && k != 13)) return 0;
    load_trophy_unlocked();
    if (k == 10) {
        if (!stats_dirty) {
            static int logged;
            if (logged < 2) { logged++; log_line("not publishing stats that have not changed"); }
            return 1;
        }
        stats_dirty = 0;
        return 0;
    }
    int id = trophy_number((const char *)(uintptr_t)a[0]);
    int earned = id >= 0 && (trophy_unlocked & ((uint64_t)1 << id));
    int same = id >= 0 && stat_have[id] && (int32_t)a[1] == stat_value[id] && (k == 4 || k == 13);
    if ((k == 7 || k == 13) && earned) same = 1;
    if (same) {
        static int logged;
        if (logged < 4) { logged++; log_line("not reporting trophy %d again", id); }
        return 1;
    }
    if (id >= 0) {
        static int logged;
        if (logged < 6) { logged++; log_line("reporting trophy %d method %d value %d", id, k, (int)a[1]); }
    }
    stats_dirty = 1;
    return 0;
}

static u64 call_method(proxy *p, int k, u64 *a) {
    if (k < 0 || k >= METHODS) return 0;
    if (skip_repeat_trophy(p, k, a)) return 1;
    if (k == 0 && !strncmp(p->name, "SteamInput00", 12)) {
        steaminput_manifest(p->mac);
        /* Let SteamAPI_RunCallbacks flush Steam Input too, instead of waiting on the
         * game to call RunFrame itself. Haptic calls below also flush immediately. */
        a[0] = 0;
    }
    void **mvt = *(void ***)p->mac;
    sysv_fn fn = (sysv_fn)mvt[k];
    int kind = special(p, k);
    int first = !p->called[k];
    p->called[k] = 1;
    u64 args[9];
    memcpy(args, a, sizeof args);
    for (int i = 0; i < 9; i++) if (in_windows_code(args[i])) args[i] = sysv_callback(args[i]);
    u64 r;
    if (kind == RET_CLASS8) {
        u64 v = fn((u64)(uintptr_t)p->mac, args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], 0);
        if (args[0] >= 0x10000) memcpy((void *)(uintptr_t)args[0], &v, 8);
        r = args[0];
    } else if (kind == RET_INPUT_DIGITAL || kind == RET_INPUT_ANALOG) {
        sysv_pair v = ((sysv_pair_fn)fn)((u64)(uintptr_t)p->mac, args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8], 0);
        uint8_t raw[16];
        int n = kind == RET_INPUT_DIGITAL ? 2 : 13;
        memcpy(raw, &v.lo, 8);
        memcpy(raw + 8, &v.hi, 8);
        if (args[0] >= 0x10000) memcpy((void *)(uintptr_t)args[0], raw, (size_t)n);
        r = args[0];
        if (!strncmp(p->name, "SteamInput00", 12) && (k == 17 || k == 21)) {
            static int shown;
            float x = 0, y = 0;
            if (k == 21) { memcpy(&x, raw + 4, 4); memcpy(&y, raw + 8, 4); }
            int hot = (k == 17 && raw[0]) || (k == 21 && (x > 0.25f || x < -0.25f || y > 0.25f || y < -0.25f));
            if (shown < 6 || (hot && shown < 40)) {
                shown++;
                log_line("%s %s handle %#llx action %#llx %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                    p->name, k == 17 ? "digital" : "analog",
                    (unsigned long long)args[1], (unsigned long long)args[2],
                    raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12]);
            }
        }
    } else if (kind == RET_INPUT_MOTION) {
        r = fn(args[0], (u64)(uintptr_t)p->mac, args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
        if (!r) r = args[0];
    } else r = fn((u64)(uintptr_t)p->mac, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
    char ver[64];
    const char *name = NULL;
    if (!strncmp(p->name, "SteamClient", 11))
        for (int i = 1; i < 3 && !name && r; i++) name = version_string(a[i], ver, sizeof ver);
    if (name && r) r = (u64)(uintptr_t)wrap((void *)(uintptr_t)r, name);
    if (first && !strncmp(p->name, "SteamInput00", 12) && k == 6)
        log_line("%s connected controllers %llu", p->name, (unsigned long long)r);
    /* Rumble and adaptive triggers are only written to the pad from RunFrame. The game
     * reads buttons from the controller cache, which stays fresh, but it can go seconds
     * between RunFrame calls. Flush now so the effect is not sitting in that queue. */
    if (!strncmp(p->name, "SteamInput00", 12) && (k == 30 || k == 31 || k == 32 || k == 47)) {
        sysv_fn run = (sysv_fn)mvt[3];
        if (run) run((u64)(uintptr_t)p->mac, 1, 0, 0, 0, 0, 0, 0, 0, 0);
        if (first) log_line("%s method %d (haptic/trigger), flushed", p->name, k);
    }
    if (g_trace && first) log_line("%s method %d = %#llx", p->name, k, (unsigned long long)r);
    return r;
}

#define STUB(k) static WINAPI u64 stub##k(proxy *p, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8, u64 a9) { \
    u64 a[9] = { a1, a2, a3, a4, a5, a6, a7, a8, a9 }; return call_method(p, k, a); }
#include "stubs.inc"
#undef STUB

WINAPI void *CreateInterface(const char *version, int *code) {
    void *lib = mac_lib();
    SYSV void *(*create)(const char *, int *) = lib ? dlsym(lib, "CreateInterface") : NULL;
    int local = 0;
    void *mac = create ? create(version, code ? code : &local) : NULL;
    log_line("CreateInterface(%s) = %p code %d", version ? version : "(null)", mac, code ? *code : local);
    return mac ? wrap(mac, version) : NULL;
}

#define FLAT(name) WINAPI u64 name(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) { \
    static sysv_fn fn; if (!fn && mac_lib()) fn = (sysv_fn)dlsym(g_lib, #name); \
    return fn ? fn(a, b, c, d, e, f, g, h, 0, 0) : 0; }
FLAT(Steam_BConnected) FLAT(Steam_BLoggedOn) FLAT(Steam_BReleaseSteamPipe) FLAT(Steam_ConnectToGlobalUser)
FLAT(Steam_CreateGlobalUser) FLAT(Steam_CreateLocalUser) FLAT(Steam_CreateSteamPipe) FLAT(Steam_FreeLastCallback)
FLAT(Steam_GSBLoggedOn) FLAT(Steam_GSBSecure) FLAT(Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient)
FLAT(Steam_GSGetSteamID) FLAT(Steam_GSLogOff) FLAT(Steam_GSLogOn) FLAT(Steam_GSRemoveUserConnect)
FLAT(Steam_GSSendSteam2UserConnect) FLAT(Steam_GSSendSteam3UserConnect) FLAT(Steam_GSSendUserDisconnect)
FLAT(Steam_GSSendUserStatusResponse) FLAT(Steam_GSSetServerType) FLAT(Steam_GSSetSpawnCount) FLAT(Steam_GSUpdateStatus)
FLAT(Steam_GetAPICallResult) FLAT(Steam_GetGSHandle) FLAT(Steam_InitiateGameConnection) FLAT(Steam_LogOff) FLAT(Steam_LogOn)
FLAT(Steam_ReleaseThreadLocalMemory) FLAT(Steam_ReleaseUser) FLAT(Steam_SetLocalIPBinding) FLAT(Steam_TerminateGameConnection)

typedef struct { int user, id; uint8_t *data; int size; } callback_msg;
static __thread uint8_t t_converted[4096];
WINAPI BOOL Steam_BGetCallback(int pipe, callback_msg *msg) {
    static sysv_fn fn;
    if (!fn && mac_lib()) fn = (sysv_fn)dlsym(g_lib, "Steam_BGetCallback");
    BOOL got = fn ? (BOOL)fn((u64)pipe, (u64)(uintptr_t)msg, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
    if (!got || !msg) return 0;
    if (msg->id == 1101 && msg->size == 20 && msg->data) {
        memset(t_converted, 0, 24);
        memcpy(t_converted, msg->data, 12);
        memcpy(t_converted + 16, msg->data + 12, 8);
        msg->data = t_converted; msg->size = 24;
    }
    if (msg->id == 1221 && msg->size >= 4 + 696 && msg->size <= 2048 && msg->data) {
        memset(t_converted, 0, 712);
        memcpy(t_converted, msg->data, 4);
        memcpy(t_converted + 8, msg->data + 4, 696);
        memcpy(t_converted + 704, msg->data + 700, 4);
        msg->data = t_converted; msg->size = 712;
    }
    return 1;
}

static int known_version(const char *version) {
    static char *text;
    static size_t size;
    if (!text) {
        Dl_info info;
        void *create = mac_lib() ? dlsym(g_lib, "CreateInterface") : NULL;
        FILE *f = create && dladdr(create, &info) ? fopen(info.dli_fname, "rb") : NULL;
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0) { text = malloc((size_t)sz + 1); size = fread(text, 1, (size_t)sz, f); text[size] = 0; }
            fclose(f);
        }
        if (!text) return 1;
    }
    size_t n = strlen(version);
    for (const char *p = text; size >= n && (p = memmem(p, size - (size_t)(p - text), version, n + 1)); p++)
        if (p == text || !p[-1]) return 1;
    return 0;
}
WINAPI BOOL Steam_IsKnownInterface(const char *version) {
    return version && known_version(version);
}
WINAPI void Steam_NotifyMissingInterface(int pipe, const char *version) {
    (void)pipe;
    log_line("Mac Steam client lacks interface %s", version ? version : "(null)");
}
WINAPI u64 Breakpad_SteamMiniDumpInit(void) { return 0; }
WINAPI u64 Breakpad_SteamSendMiniDump(void) { return 0; }
WINAPI u64 Breakpad_SteamSetAppID(void) { return 0; }
WINAPI u64 Breakpad_SteamSetSteamID(void) { return 0; }
WINAPI u64 Breakpad_SteamWriteMiniDumpSetComment(void) { return 0; }
WINAPI u64 Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(void) { return 0; }

/* kernel32 / advapi32 resolved from the process, so this library does not import them. */
typedef WINAPI void *(*k32_load)(const WCHAR *);
typedef WINAPI void *(*k32_proc)(void *, const char *);
typedef WINAPI DWORD (*k32_pid)(void);
typedef WINAPI void (*k32_sleep)(DWORD);
typedef WINAPI long (*reg_create)(void *, const WCHAR *, DWORD, void *, DWORD, DWORD, void *, void **, DWORD *);
typedef WINAPI long (*reg_set)(void *, const WCHAR *, DWORD, DWORD, const void *, DWORD);
typedef WINAPI long (*reg_close)(void *);

static void *pe_export(uint8_t *base, const char *name) {
    if (!base || *(uint16_t *)base != 0x5a4d) return NULL;
    int32_t lfanew = *(int32_t *)(base + 0x3c);
    uint8_t *nt = base + lfanew;
    uint32_t exp_rva = *(uint32_t *)(nt + 24 + 112);
    if (!exp_rva) return NULL;
    uint8_t *exp = base + exp_rva;
    uint32_t nnames = *(uint32_t *)(exp + 24);
    uint32_t funcs = *(uint32_t *)(exp + 28);
    uint32_t names = *(uint32_t *)(exp + 32);
    uint32_t ords = *(uint32_t *)(exp + 36);
    for (uint32_t i = 0; i < nnames; i++) {
        const char *nm = (const char *)(base + *(uint32_t *)(base + names + i * 4));
        if (!strcmp(nm, name)) {
            uint16_t ord = *(uint16_t *)(base + ords + i * 2);
            return base + *(uint32_t *)(base + funcs + ord * 4);
        }
    }
    return NULL;
}
static uint8_t *find_module(const WCHAR *name) {
    void *teb;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(teb));
    if (!teb) return NULL;
    uint8_t *peb = *(uint8_t **)((uint8_t *)teb + 0x60);
    uint8_t *ldr = peb ? *(uint8_t **)(peb + 0x18) : NULL;
    if (!ldr) return NULL;
    uint8_t *head = ldr + 0x20;
    for (uint8_t *e = *(uint8_t **)head; e && e != head; e = *(uint8_t **)e) {
        ustr base_name;
        memcpy(&base_name, e + 0x48, sizeof base_name);
        if (!base_name.buf) continue;
        int i = 0;
        for (; name[i]; i++) {
            WCHAR c = base_name.buf[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            WCHAR w = name[i];
            if (w >= 'A' && w <= 'Z') w += 32;
            if (c != w) break;
        }
        if (!name[i] && !base_name.buf[i]) return *(uint8_t **)(e + 0x20);
    }
    return NULL;
}
static void publish_process(void) {
    static const WCHAR k32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
    static const WCHAR adv[] = {'a','d','v','a','p','i','3','2','.','d','l','l',0};
    static const WCHAR sub[] = {'S','o','f','t','w','a','r','e','\\','V','a','l','v','e','\\','S','t','e','a','m','\\','A','c','t','i','v','e','P','r','o','c','e','s','s',0};
    static const WCHAR pid_n[] = {'p','i','d',0};
    static const WCHAR user_n[] = {'A','c','t','i','v','e','U','s','e','r',0};
    static const WCHAR dll_n[] = {'S','t','e','a','m','C','l','i','e','n','t','D','l','l','6','4',0};
    static const WCHAR dll[] = {'C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m','3','2','\\','s','t','e','a','m','c','l','i','e','n','t','6','4','.','d','l','l',0};
    static const WCHAR steamkey[] = {'S','o','f','t','w','a','r','e','\\','V','a','l','v','e','\\','S','t','e','a','m',0};
    static const WCHAR path_n[] = {'S','t','e','a','m','P','a','t','h',0};
    static const WCHAR dir[] = {'C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m','3','2',0};
    uint8_t *kernel = find_module(k32);
    log_line("registry: kernel32 %p", kernel);
    if (!kernel) return;
    k32_load load = (k32_load)pe_export(kernel, "LoadLibraryW");
    k32_proc proc = (k32_proc)pe_export(kernel, "GetProcAddress");
    k32_pid pidfn = (k32_pid)pe_export(kernel, "GetCurrentProcessId");
    log_line("registry: load %p proc %p pid %p", (void *)load, (void *)proc, (void *)pidfn);
    if (!load || !proc || !pidfn) return;
    void *advapi = load(adv);
    log_line("registry: advapi32 %p", advapi);
    if (!advapi) return;
    reg_create create = (reg_create)proc(advapi, "RegCreateKeyExW");
    reg_set set = (reg_set)proc(advapi, "RegSetValueExW");
    reg_close closek = (reg_close)proc(advapi, "RegCloseKey");
    log_line("registry: create %p set %p close %p", (void *)create, (void *)set, (void *)closek);
    if (!create || !set || !closek) return;
    void *key = NULL;
    long st = create((void *)(uintptr_t)0x80000001, sub, 0, NULL, 0, 0x20006, NULL, &key, NULL);
    if (st || !key) { log_line("RegCreateKeyEx %ld", st); return; }
    DWORD pid = pidfn(), user = 1;
    set(key, pid_n, 0, 4, &pid, 4);
    set(key, user_n, 0, 4, &user, 4);
    set(key, dll_n, 0, 1, dll, (DWORD)(sizeof dll));
    closek(key);
    void *steam = NULL;
    if (!create((void *)(uintptr_t)0x80000001, steamkey, 0, NULL, 0, 0x20006, NULL, &steam, NULL) && steam) {
        set(steam, path_n, 0, 1, dir, (DWORD)(sizeof dir));
        closek(steam);
    }
    log_line("ActiveProcess pid %u", pid);
}

WINAPI void SteamBridgeWait(void *hwnd, void *inst, char *cmd, int show) {
    (void)hwnd; (void)inst; (void)cmd; (void)show;
    /* Registry writes from this unix callback deadlock in advapi32, so the launcher
     * records the pid below with reg.exe. This process only has to stay alive. */
    static const WCHAR k32[] = {'k','e','r','n','e','l','3','2','.','d','l','l',0};
    uint8_t *kernel = find_module(k32);
    k32_pid pidfn = kernel ? (k32_pid)pe_export(kernel, "GetCurrentProcessId") : NULL;
    k32_sleep sleepfn = kernel ? (k32_sleep)pe_export(kernel, "Sleep") : NULL;
    DWORD pid = pidfn ? pidfn() : 0;
    log_line("ActiveProcess pid %u", pid);
    for (;;) if (sleepfn) sleepfn(0xffffffff); else pause();
}
typedef WINAPI u64 (*stubfn)(void *, u64, u64, u64, u64, u64, u64, u64, u64, u64);
static u64 vcall(void *obj, int k, u64 a0, u64 a1, u64 a2) {
    void **vt = *(void ***)obj;
    return ((stubfn)vt[k])(obj, a0, a1, a2, 0, 0, 0, 0, 0, 0);
}
WINAPI void SteamBridgeProbe(void *hwnd, void *inst, char *cmd, int show) {
    (void)hwnd; (void)inst; (void)cmd; (void)show;
    int code = 0;
    /* DS2's steam_api64 asks for SteamClient020, then GetISteamInput(..., "SteamInput006"). */
    void *client = CreateInterface("SteamClient020", &code);
    if (!client) { log_line("probe: no SteamClient020"); return; }
    u64 pipe = vcall(client, 0, 0, 0, 0);
    u64 user = vcall(client, 2, pipe, 0, 0);
    void *input = (void *)(uintptr_t)vcall(client, 38, user, pipe, (u64)(uintptr_t)"SteamInput006");
    log_line("probe pipe %llu user %llu input %p", (unsigned long long)pipe, (unsigned long long)user, input);
    if (!input) return;
    const char *manifest = getenv("DS2_STEAM_MANIFEST");
    if (manifest && manifest[0]) log_line("probe manifest %llu", (unsigned long long)vcall(input, 2, (u64)(uintptr_t)manifest, 0, 0));
    u64 inited = vcall(input, 0, 0, 0, 0);
    u64 menu = vcall(input, 9, (u64)(uintptr_t)"Menu", 0, 0);
    u64 ingame = vcall(input, 9, (u64)(uintptr_t)"InGame", 0, 0);
    log_line("probe Init %llu Menu %#llx InGame %#llx", (unsigned long long)inited, (unsigned long long)menu, (unsigned long long)ingame);
    u64 handles[16];
    u64 n = 0;
    for (int t = 0; t < 30 && !n; t++) {
        vcall(input, 3, 0, 0, 0);
        memset(handles, 0, sizeof handles);
        n = vcall(input, 6, (u64)(uintptr_t)handles, 0, 0);
        if (!n && handles[0]) n = 1;
        if (!n) usleep(100000);
    }
    log_line("probe controllers %llu", (unsigned long long)n);
    for (u64 i = 0; i < n && i < 16; i++) {
        u64 type = vcall(input, 37, handles[i], 0, 0);
        log_line("probe controller %llu handle %#llx type %llu", (unsigned long long)i, (unsigned long long)handles[i], (unsigned long long)type);
    }
    if (n) {
        u64 ingame = vcall(input, 9, (u64)(uintptr_t)"InGame", 0, 0);
        vcall(input, 10, handles[0], ingame, 0);
        vcall(input, 3, 0, 0, 0);
        const char *analogs[] = { "SteamLook", "SteamMove", "SteamGyro" };
        for (int i = 0; i < 3; i++) {
            u64 h = vcall(input, 20, (u64)(uintptr_t)analogs[i], 0, 0);
            unsigned char raw[16];
            memset(raw, 0, sizeof raw);
            vcall(input, 21, (u64)(uintptr_t)raw, handles[0], h);
            log_line("probe analog %s", analogs[i]);
            log_line("  %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11], raw[12]);
        }
        unsigned char motion[64];
        memset(motion, 0x5a, sizeof motion);
        vcall(input, 29, (u64)(uintptr_t)motion, handles[0], 0);
        log_line("probe motion %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
            motion[0], motion[1], motion[2], motion[3], motion[4], motion[5], motion[6], motion[7],
            motion[8], motion[9], motion[10], motion[11], motion[12], motion[13], motion[14], motion[15]);
    }
}

static void init_vtbl(void) {
    static const void *stubs[] = {
#define STUB(k) (void *)stub##k,
#include "stubs.inc"
#undef STUB
    };
    for (int i = 0; i < METHODS; i++) g_vtbl[i] = (void *)stubs[i];
    g_trace = getenv("VXRT_TRACE_STEAM") ? atoi(getenv("VXRT_TRACE_STEAM")) : 0;
}

BOOL WINAPI DllMain(void *inst, DWORD reason, void *reserved) {
    (void)inst; (void)reserved;
    if (reason == 1) init_vtbl();
    return 1;
}

/* Called from the Windows steamclient64.dll through NtQueryVirtualMemory / __wine_unix_call. */
/* Same layout as pe_bridge.c's struct args. The return slot has to sit where that DLL reads it. */
struct bridge_args { u64 a[8]; u64 ret; };
static int uc_ret(void *p, u64 v) { ((struct bridge_args *)p)->ret = v; return 0; }
static int uc_create(void *p) { struct bridge_args *A = p; init_vtbl(); return uc_ret(p, (u64)(uintptr_t)CreateInterface((const char *)(uintptr_t)A->a[0], (int *)(uintptr_t)A->a[1])); }
#define UFLAT(fn) static int uc_##fn(void *p) { struct bridge_args *A = p; init_vtbl(); return uc_ret(p, fn(A->a[0], A->a[1], A->a[2], A->a[3], A->a[4], A->a[5], A->a[6], A->a[7])); }
UFLAT(Steam_BConnected) UFLAT(Steam_BLoggedOn) UFLAT(Steam_BReleaseSteamPipe) UFLAT(Steam_ConnectToGlobalUser)
UFLAT(Steam_CreateGlobalUser) UFLAT(Steam_CreateLocalUser) UFLAT(Steam_CreateSteamPipe) UFLAT(Steam_FreeLastCallback)
UFLAT(Steam_GSBLoggedOn) UFLAT(Steam_GSBSecure) UFLAT(Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient)
UFLAT(Steam_GSGetSteamID) UFLAT(Steam_GSLogOff) UFLAT(Steam_GSLogOn) UFLAT(Steam_GSRemoveUserConnect)
UFLAT(Steam_GSSendSteam2UserConnect) UFLAT(Steam_GSSendSteam3UserConnect) UFLAT(Steam_GSSendUserDisconnect)
UFLAT(Steam_GSSendUserStatusResponse) UFLAT(Steam_GSSetServerType) UFLAT(Steam_GSSetSpawnCount) UFLAT(Steam_GSUpdateStatus)
UFLAT(Steam_GetAPICallResult) UFLAT(Steam_GetGSHandle) UFLAT(Steam_InitiateGameConnection) UFLAT(Steam_LogOff) UFLAT(Steam_LogOn)
UFLAT(Steam_ReleaseThreadLocalMemory) UFLAT(Steam_ReleaseUser) UFLAT(Steam_SetLocalIPBinding) UFLAT(Steam_TerminateGameConnection)
static int uc_getcb(void *p) { struct bridge_args *A = p; init_vtbl(); return uc_ret(p, (u64)Steam_BGetCallback((int)A->a[0], (callback_msg *)(uintptr_t)A->a[1])); }
static int uc_known(void *p) { struct bridge_args *A = p; init_vtbl(); return uc_ret(p, (u64)Steam_IsKnownInterface((const char *)(uintptr_t)A->a[0])); }
static int uc_missing(void *p) { struct bridge_args *A = p; init_vtbl(); Steam_NotifyMissingInterface((int)A->a[0], (const char *)(uintptr_t)A->a[1]); return uc_ret(p, 0); }
static int uc_bp(void *p) { (void)p; return uc_ret(p, 0); }
static int uc_wait(void *p) { struct bridge_args *A = p; init_vtbl(); SteamBridgeWait((void *)(uintptr_t)A->a[0], (void *)(uintptr_t)A->a[1], (char *)(uintptr_t)A->a[2], (int)A->a[3]); return uc_ret(p, 0); }
static int uc_probe(void *p) { struct bridge_args *A = p; init_vtbl(); SteamBridgeProbe((void *)(uintptr_t)A->a[0], (void *)(uintptr_t)A->a[1], (char *)(uintptr_t)A->a[2], (int)A->a[3]); return uc_ret(p, 0); }

__attribute__((visibility("default")))
const void *__wine_unix_call_funcs[] = {
    uc_create,
    uc_Steam_BConnected, uc_getcb, uc_Steam_BLoggedOn, uc_Steam_BReleaseSteamPipe, uc_Steam_ConnectToGlobalUser,
    uc_Steam_CreateGlobalUser, uc_Steam_CreateLocalUser, uc_Steam_CreateSteamPipe, uc_Steam_FreeLastCallback,
    uc_Steam_GSBLoggedOn, uc_Steam_GSBSecure, uc_Steam_GSGetSteam2GetEncryptionKeyToSendToNewClient,
    uc_Steam_GSGetSteamID, uc_Steam_GSLogOff, uc_Steam_GSLogOn, uc_Steam_GSRemoveUserConnect,
    uc_Steam_GSSendSteam2UserConnect, uc_Steam_GSSendSteam3UserConnect, uc_Steam_GSSendUserDisconnect,
    uc_Steam_GSSendUserStatusResponse, uc_Steam_GSSetServerType, uc_Steam_GSSetSpawnCount, uc_Steam_GSUpdateStatus,
    uc_Steam_GetAPICallResult, uc_Steam_GetGSHandle, uc_Steam_InitiateGameConnection, uc_known, uc_Steam_LogOff, uc_Steam_LogOn,
    uc_missing, uc_Steam_ReleaseThreadLocalMemory, uc_Steam_ReleaseUser, uc_Steam_SetLocalIPBinding, uc_Steam_TerminateGameConnection,
    uc_wait, uc_probe,
    uc_bp, uc_bp, uc_bp, uc_bp, uc_bp, uc_bp,
};
