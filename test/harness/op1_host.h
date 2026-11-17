/* PC stand-ins for the firmware addresses and calls op1_plugin.h uses (included by it under -DOP1_HOST).
 * The audio side is plain arrays; the screen side records what the plugin draws, for tyr8_test.c to check and dump. */
#ifndef OP1_HOST_H
#define OP1_HOST_H
#include <stdint.h>

extern int16_t  host_sine[8192];
extern int32_t  host_pitch_inc[128];
extern uint8_t  host_voice_mem[6 * 0x24];
extern int16_t  host_voice_vel[6];
extern int16_t  host_voice_env[128];
extern int16_t  host_knobs[8][8];
extern int      host_slot;

#define OP1_PITCH_INC    ((const int32_t *)host_pitch_inc)
#define OP1_SINE_TABLE   ((const int16_t *)host_sine)
#define OP1_VOICE_BASE   ((uintptr_t)host_voice_mem)
#define OP1_VOICE_VEL    ((const int16_t *)host_voice_vel)
#define OP1_VOICE_ENV    ((const int16_t *)host_voice_env)
#define OP1_SCREEN_SLOT  host_slot

void *host_find_node(void *scr, const char *id);
void  host_node_set_transform(void *node, const int32_t *m9);
void  host_screen_draw(void *scr, int dt);
void *host_raster(void);
void  host_raster_line(void *r, const void *p0, const void *p1, int32_t w_q10, const uint8_t *rgba);
#define op1_find_node(scr, id)            host_find_node((scr), (id))
#define op1_node_set_transform(node, m9)  host_node_set_transform((node), (m9))
#define op1_screen_draw(scr, dt)          host_screen_draw((scr), (dt))
#define op1_raster()                      host_raster()
#define op1_raster_line(r, p0, p1, w, c)  host_raster_line((r), (p0), (p1), (w), (c))

static inline int op1_current_slot(void) { return host_slot; }
static inline const int16_t *op1_synth_knobs(int slot) { return host_knobs[slot & 7]; }
static inline void op1_synth_knobs_live(int slot, int16_t out[4])
{ for (int i = 0; i < 4; i++) out[i] = host_knobs[slot & 7][i]; }
#endif
