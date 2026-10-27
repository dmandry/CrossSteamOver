/* Write DualSense rumble and adaptive-trigger reports over IOHID.
 * Steam's own output path waits until it has switched the pad back from the
 * desktop configuration, which is several seconds. This write does not.
 * A Bluetooth report without the 0x10 tag is ignored, and a report that sets
 * the "classic rumble" select bit also switches off the audio haptic channel.
 * Repeating that bit cleared vibrations Steam had not played yet. */
#include "dualsense_out.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEV 4

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_t g_thread;
static int g_started;
static IOHIDDeviceRef g_dev[MAX_DEV];
static int g_dev_n;
static int g_bt[MAX_DEV];
static uint8_t g_seq;
static uint8_t g_left, g_right;
static uint8_t g_lfx[11], g_rfx[11];
static int g_dirty;

static uint32_t crc32_step(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

/* Steam's encoder (steamclient, arm64 0x7bb564) writes the mode at byte 1 of
 * each 11-byte block. Byte 0 stays 0. Zones follow at bytes 2-3, then six
 * bytes of 3-bit strengths, and a vibration frequency at byte 10. */
static uint8_t clamp8(uint8_t v, uint8_t max) { return v > max ? max : v; }

static void trigger_off(uint8_t out[11]) {
    memset(out, 0, 11);
    out[1] = 0x05;
}

static void pack_strengths(uint8_t out[11], uint8_t mode, const uint8_t strength[10], uint8_t frequency) {
    uint64_t packed = 0;
    uint16_t active = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t s = strength[i];
        if (s > 8) s = 8;
        if (s == 0) continue;
        packed |= (uint64_t)((s - 1) & 7) << (3 * i);
        active |= (uint16_t)1 << i;
    }
    memset(out, 0, 11);
    out[1] = active ? mode : 0x05;
    out[2] = (uint8_t)active;
    out[3] = (uint8_t)(active >> 8);
    for (int i = 0; i < 6; i++) out[4 + i] = (uint8_t)(packed >> (8 * i));
    out[10] = frequency;
}

static void effect_from_here(uint8_t out[11], uint8_t mode, uint8_t position, uint8_t strength, uint8_t frequency) {
    uint8_t s[10];
    memset(s, 0, sizeof s);
    position = clamp8(position, 9);
    strength = clamp8(strength, 8);
    if (strength == 0) {
        trigger_off(out);
        return;
    }
    for (int i = position; i < 10; i++) s[i] = strength;
    pack_strengths(out, mode, s, frequency);
}

/* command points at the SCE command: u32 mode, 4 bytes pad, then the payload. */
static void encode_trigger(const uint8_t *command, uint8_t out[11]) {
    uint32_t mode = (uint32_t)command[0] | ((uint32_t)command[1] << 8) |
                    ((uint32_t)command[2] << 16) | ((uint32_t)command[3] << 24);
    const uint8_t *d = command + 8;
    uint8_t strengths[10];
    memset(out, 0, 11);
    switch (mode) {
    case 1: /* feedback: position, strength. Resistance from there to the end. */
        effect_from_here(out, 0x21, d[0], d[1], 0);
        break;
    case 2: { /* weapon: start, end, strength. Mode at byte 1, zones at 2, strength at 4. */
        uint8_t start = clamp8(d[0], 9), end = clamp8(d[1], 9), strength = clamp8(d[2], 8);
        uint16_t zones = (uint16_t)((1u << start) | (1u << end));
        memset(out, 0, 11);
        out[1] = 0x25;
        out[2] = (uint8_t)zones;
        out[3] = (uint8_t)(zones >> 8);
        out[4] = strength ? (uint8_t)(strength - 1) : 0;
        break;
    }
    case 3: /* vibration: position, amplitude, frequency */
        effect_from_here(out, 0x26, d[0], d[1], d[2]);
        break;
    case 4: /* ten resistance points */
        memcpy(strengths, d, 10);
        pack_strengths(out, 0x21, strengths, 0);
        break;
    case 5: { /* slope: start, end, start strength, end strength */
        uint8_t start = clamp8(d[0], 9), end = clamp8(d[1], 9);
        uint8_t a = clamp8(d[2], 8), b = clamp8(d[3], 8);
        uint16_t zones = (uint16_t)((1u << start) | (1u << end));
        uint8_t low = a ? (uint8_t)(a - 1) : 0;
        uint8_t pair = (uint8_t)((((b << 3) + 0x38) & 0x38) | (low & 7));
        memset(out, 0, 11);
        out[1] = 0x22;
        out[2] = (uint8_t)zones;
        out[3] = (uint8_t)(zones >> 8);
        out[4] = pair;
        break;
    }
    case 6: /* frequency, then ten amplitudes */
        memcpy(strengths, d + 1, 10);
        pack_strengths(out, 0x26, strengths, d[0]);
        break;
    default:
        trigger_off(out);
        break;
    }
}

static int motors_on(void) { return g_left || g_right; }

static int trigger_programmed(const uint8_t fx[11]) { return fx[1] != 0; }

static void fill_state(uint8_t state[47]) {
    memset(state, 0, 47);
    /* Grip motors use byte 0. Trigger enables are byte 1: 0x04 right, 0x08 left.
     * Steam also sets byte 0 to 0x02 whenever a trigger block is sent. */
    if (motors_on()) state[0] |= 0x01 | 0x02;
    if (trigger_programmed(g_rfx)) state[1] |= 0x04;
    if (trigger_programmed(g_lfx)) state[1] |= 0x08;
    if (state[1] & 0x0c) state[0] |= 0x02;
    state[2] = g_right;
    state[3] = g_left;
    memcpy(state + 10, g_rfx, 11);
    memcpy(state + 21, g_lfx, 11);
}

static void note_report(int bt, IOReturn status) {
    static int logged;
    if (logged) return;
    logged = 1;
    FILE *f = fopen("/tmp/steambridge.log", "a");
    if (!f) return;
    fprintf(f, "dualsense output %s status %#x\n", bt ? "bluetooth" : "usb", status);
    fclose(f);
}
static void send_one(IOHIDDeviceRef dev, int bt, const uint8_t state[47]) {
    if (!bt) {
        uint8_t pkt[48];
        pkt[0] = 0x02;
        memcpy(pkt + 1, state, 47);
        IOReturn st = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x02, pkt, sizeof pkt);
        if (st != kIOReturnSuccess)
            st = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x02, pkt + 1, 47);
        note_report(0, st);
        return;
    }
    uint8_t pkt[78];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x31;
    /* High nibble is a sequence the pad drops duplicates of. Byte 2 has to
     * be 0x10 or the controller ignores the report. The 47-byte state follows. */
    pkt[1] = (uint8_t)((g_seq & 0x0f) << 4);
    pkt[2] = 0x10;
    g_seq = (uint8_t)((g_seq + 1) & 0x0f);
    memcpy(pkt + 3, state, 47);
    uint8_t prefix = 0xA2;
    uint32_t crc = crc32_step(0, &prefix, 1);
    crc = crc32_step(crc, pkt, 74);
    pkt[74] = (uint8_t)crc;
    pkt[75] = (uint8_t)(crc >> 8);
    pkt[76] = (uint8_t)(crc >> 16);
    pkt[77] = (uint8_t)(crc >> 24);
    IOReturn st = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x31, pkt, sizeof pkt);
    if (st != kIOReturnSuccess)
        st = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, 0x31, pkt + 1, 77);
    note_report(1, st);
}

static void send_all(const uint8_t state[47]) {
    for (int i = 0; i < g_dev_n; i++)
        if (g_dev[i]) send_one(g_dev[i], g_bt[i], state);
}

static int transport_is_bt(IOHIDDeviceRef dev) {
    CFTypeRef t = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDTransportKey));
    if (!t || CFGetTypeID(t) != CFStringGetTypeID()) return 1;
    return CFStringFind((CFStringRef)t, CFSTR("Bluetooth"), 0).location != kCFNotFound ||
           CFStringFind((CFStringRef)t, CFSTR("BluetoothLowEnergy"), 0).location != kCFNotFound;
}

static void open_devices(void) {
    int pids[2] = { 0x0ce6, 0x0df2 };
    for (int p = 0; p < 2 && g_dev_n < MAX_DEV; p++) {
        int vid = 0x054c, pid = pids[p];
        CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid);
        CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
        CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), v);
        CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), n);
        CFRelease(v);
        CFRelease(n);
        IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        IOHIDManagerSetDeviceMatching(mgr, match);
        CFRelease(match);
        if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
            CFRelease(mgr);
            continue;
        }
        CFSetRef set = IOHIDManagerCopyDevices(mgr);
        if (!set) { CFRelease(mgr); continue; }
        CFIndex count = CFSetGetCount(set);
        IOHIDDeviceRef buf[8];
        if (count > 8) count = 8;
        CFSetGetValues(set, (const void **)buf);
        for (CFIndex i = 0; i < count && g_dev_n < MAX_DEV; i++) {
            if (IOHIDDeviceOpen(buf[i], kIOHIDOptionsTypeNone) != kIOReturnSuccess) continue;
            CFRetain(buf[i]);
            g_bt[g_dev_n] = transport_is_bt(buf[i]);
            g_dev[g_dev_n++] = buf[i];
        }
        CFRelease(set);
        /* The manager has to stay alive for the devices to keep working. */
        (void)mgr;
    }
    FILE *f = fopen("/tmp/steambridge.log", "a");
    if (f) {
        fprintf(f, "dualsense devices %d%s\n", g_dev_n, g_dev_n && g_bt[0] ? " bluetooth" : "");
        fclose(f);
    }
}

static void *writer(void *unused) {
    (void)unused;
    open_devices();
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_dirty)
            pthread_cond_wait(&g_wake, &g_lock);
        uint8_t state[47];
        fill_state(state);
        g_dirty = 0;
        pthread_mutex_unlock(&g_lock);
        /* One report per change. Repeating the same effect restarts the
         * trigger motor and it ticks, which is the cracking sound. */
        send_all(state);
    }
    return NULL;
}

static void ensure_thread(void) {
    if (g_started) return;
    g_started = 1;
    memset(g_lfx, 0, sizeof g_lfx);
    memset(g_rfx, 0, sizeof g_rfx);
    g_lfx[1] = g_rfx[1] = 0x05;
    pthread_create(&g_thread, NULL, writer, NULL);
}

void dualsense_rumble(uint16_t left, uint16_t right) {
    uint8_t nl = (uint8_t)(left >> 8), nr = (uint8_t)(right >> 8);
    ensure_thread();
    pthread_mutex_lock(&g_lock);
    if (nl != g_left || nr != g_right) {
        g_left = nl;
        g_right = nr;
        g_dirty = 1;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

void dualsense_triggers(const void *param, unsigned size) {
    if (!param || size < 8 + 56) return;
    ensure_thread();
    const uint8_t *p = param;
    uint8_t mask = p[0];
    uint8_t lfx[11], rfx[11];
    pthread_mutex_lock(&g_lock);
    memcpy(lfx, g_lfx, 11);
    memcpy(rfx, g_rfx, 11);
    if (mask & 0x01) encode_trigger(p + 8, lfx);
    if (mask & 0x02) encode_trigger(p + 8 + 56, rfx);
    if (memcmp(lfx, g_lfx, 11) != 0 || memcmp(rfx, g_rfx, 11) != 0) {
        memcpy(g_lfx, lfx, 11);
        memcpy(g_rfx, rfx, 11);
        g_dirty = 1;
        pthread_cond_signal(&g_wake);
    }
    pthread_mutex_unlock(&g_lock);
}

#ifdef DUALSENSE_TEST
int main(void) {
    fprintf(stderr, "pulse\n");
    dualsense_rumble(40000, 40000);
    usleep(400000);
    dualsense_rumble(0, 0);
    usleep(100000);
    fprintf(stderr, "devices %d\n", g_dev_n);
    return 0;
}
#endif
