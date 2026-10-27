/* Write DualSense rumble and adaptive-trigger reports over IOHID.
 * Steam's own output path waits until it has switched the pad back from the
 * desktop configuration, which is several seconds. This write does not. */
#include "dualsense_out.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEV 4

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
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

static int is_off(const uint8_t fx[11]) {
    return fx[0] == 0 || fx[0] == 0x05;
}

/* Sony's 11-byte trigger block. command points at the SCE command:
 * u32 mode, 4 bytes pad, then the mode's payload. */
static void encode_trigger(const uint8_t *command, uint8_t out[11]) {
    uint32_t mode = (uint32_t)command[0] | ((uint32_t)command[1] << 8) |
                    ((uint32_t)command[2] << 16) | ((uint32_t)command[3] << 24);
    const uint8_t *d = command + 8;
    memset(out, 0, 11);
    switch (mode) {
    case 1: /* feedback: position, strength */
        out[0] = 0x21; out[1] = d[0]; out[2] = d[1]; break;
    case 2: /* weapon: start, end, strength */
        out[0] = 0x25; out[1] = d[0]; out[2] = d[1]; out[3] = d[2]; break;
    case 3: /* vibration: position, amplitude, frequency */
        out[0] = 0x26; out[1] = d[2]; out[2] = d[1]; out[3] = d[0]; break;
    case 4: /* multi-position feedback: 10 strengths */
        out[0] = 0x21; out[1] = d[0]; break;
    case 5: /* slope feedback */
        out[0] = 0x22; out[1] = d[0]; out[2] = d[1]; out[3] = d[2]; out[4] = d[3]; break;
    case 6: /* multi-position vibration */
        out[0] = 0x26; out[1] = d[0]; out[2] = d[1]; break;
    default:
        out[0] = 0x05; break;
    }
}

static void fill_state(uint8_t state[47]) {
    memset(state, 0, 47);
    state[0] = 0x01 | 0x02; /* compatible rumble + haptic motors */
    if (!is_off(g_rfx)) state[0] |= 0x04;
    if (!is_off(g_lfx)) state[0] |= 0x08;
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
static void send_one(IOHIDDeviceRef dev, int bt) {
    uint8_t state[47];
    fill_state(state);
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
    pkt[1] = (uint8_t)((g_seq++ & 0x0f) << 4);
    memcpy(pkt + 2, state, 47);
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

static void send_all(void) {
    for (int i = 0; i < g_dev_n; i++)
        if (g_dev[i]) send_one(g_dev[i], g_bt[i]);
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
}

static void *writer(void *unused) {
    (void)unused;
    open_devices();
    for (;;) {
        usleep(16000);
        pthread_mutex_lock(&g_lock);
        int dirty = g_dirty;
        int live = g_left || g_right || !is_off(g_lfx) || !is_off(g_rfx);
        if (dirty || live) {
            g_dirty = 0;
            pthread_mutex_unlock(&g_lock);
            send_all();
        } else {
            pthread_mutex_unlock(&g_lock);
        }
    }
    return NULL;
}

static void ensure_thread(void) {
    if (g_started) return;
    g_started = 1;
    memset(g_lfx, 0, sizeof g_lfx);
    memset(g_rfx, 0, sizeof g_rfx);
    g_lfx[0] = g_rfx[0] = 0x05;
    pthread_create(&g_thread, NULL, writer, NULL);
}

void dualsense_rumble(uint16_t left, uint16_t right) {
    ensure_thread();
    pthread_mutex_lock(&g_lock);
    g_left = (uint8_t)(left >> 8);
    g_right = (uint8_t)(right >> 8);
    g_dirty = 1;
    pthread_mutex_unlock(&g_lock);
}

void dualsense_triggers(const void *param, unsigned size) {
    if (!param || size < 8 + 56) return;
    ensure_thread();
    const uint8_t *p = param;
    uint8_t mask = p[0];
    pthread_mutex_lock(&g_lock);
    if (mask & 0x01) encode_trigger(p + 8, g_lfx);
    if (mask & 0x02) encode_trigger(p + 8 + 56, g_rfx);
    g_dirty = 1;
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
