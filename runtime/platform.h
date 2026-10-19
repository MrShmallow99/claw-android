/* Host platform services used by the Win32 emulation layer (implemented in platform_sdl.c). */
#pragma once
#include <stdint.h>

int platform_init(int argc, char **argv);
void platform_shutdown(void);

/* Process pending host events (input, window lifecycle). Called from the guest message pump. */
void platform_pump_events(void);
/* Block until any host event arrives or timeout_ms passes. */
void platform_wait_events(int timeout_ms);
/* Returns nonzero when the user asked to quit. */
int platform_quit_requested(void);
/* Returns nonzero while the app is in the foreground. */
int platform_app_active(void);

/* Present a 16-bit RGB555/RGB565 or 8-bit-paletted frame produced by the game. */
enum { PIX_RGB555, PIX_RGB565, PIX_XRGB8888 };
void platform_present(const void *pixels, int w, int h, int pitch, int format);

/* Time */
uint32_t platform_ticks_ms(void);
uint64_t platform_ticks_us(void);
void platform_sleep_ms(uint32_t ms);

/* Paths */
const char *platform_game_dir(void);   /* read-only original game files */
const char *platform_save_dir(void);   /* writable overlay */

/* Audio: the mixer callback produces interleaved stereo s16 at `rate`. */
typedef void (*AudioMixFn)(int16_t *out, int frames);
int platform_audio_open(int rate, AudioMixFn fn);
void platform_audio_lock(void);
void platform_audio_unlock(void);

void platform_fatal_message(const char *msg);
int platform_message_box(const char *caption, const char *text);

/* Input (input.c): Windows virtual-key state, fed by keyboard, touch overlay and gamepads. */
void input_set_vk(int vk, int source, int down);
int input_vk_down(int vk);
void input_clear_source(int source);
void input_init(void);              /* reads [input] from game.ini */
void input_note_poll(int vk);
void input_dir_vks(int out[4]);     /* up, down, left, right */
int input_in_gameplay(void);
enum { INPUT_SRC_KEYBOARD = 0, INPUT_SRC_TOUCH = 1, INPUT_SRC_PAD = 2, INPUT_SRC_COUNT = 3 };
/* a VK Windows leaves undefined, used for the port's own "open the cheat menu" action */
#define PORT_VK_CHEATS 0x07

/* a file the port supplies itself (APK assets/port/, or port_data/ beside the binary); malloc'd */
void *platform_read_port_file(const char *name, int64_t *size);
