#ifndef DUALSENSE_OUT_H
#define DUALSENSE_OUT_H
#include <stdint.h>

/* Latest rumble (0-65535 per motor) and the 120-byte ScePadTriggerEffectParam.
 * Effects are written to the pad directly, so they are not waiting on Steam's
 * controller-config switch. */
void dualsense_rumble(uint16_t left, uint16_t right);
void dualsense_triggers(const void *param, unsigned size);
#endif
