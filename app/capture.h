// SwitchView capture interface:
// - On Linux: V4L2 video (mmap, newest-frame-wins) + ALSA audio.
// - On Windows: Windows Media Foundation (IMFSourceReader) + SDL2 audio (WASAPI),
//   with built-in synthetic test pattern generator fallback.

#ifndef SV_CAPTURE_H
#define SV_CAPTURE_H

#include <SDL2/SDL.h>
#include <stdint.h>

// Pixel formats (FourCC)
#define SV_FMT_NV12 0x3231564e  // 'NV12'
#define SV_FMT_YUYV 0x56595559  // 'YUYV'

// Compatibility aliases for code previously expecting V4L2 macro names
#ifndef V4L2_PIX_FMT_NV12
#define V4L2_PIX_FMT_NV12 SV_FMT_NV12
#endif
#ifndef V4L2_PIX_FMT_YUYV
#define V4L2_PIX_FMT_YUYV SV_FMT_YUYV
#endif

struct capture_opts {
  const char *video_dev, *audio_dev;
  int width, height, fps;
  unsigned fourcc;  // SV_FMT_NV12 or SV_FMT_YUYV
  int no_audio;
  int audio_max_ms;  // drop input beyond this much queued output
};

struct capture;

struct capture *capture_open(const struct capture_opts *o, SDL_Renderer *ren);
void capture_close(struct capture *c);

// Waits up to timeout_ms for a frame; uploads the newest one into the texture.
// Returns 1 if the texture was updated.
int capture_poll(struct capture *c, int timeout_ms);

SDL_Texture *capture_texture(struct capture *c);
unsigned capture_width(struct capture *c);
unsigned capture_height(struct capture *c);
int capture_has_audio(struct capture *c);
double capture_audio_queued_ms(struct capture *c);

// Grayscale (luma) snapshots for OCR. Any thread may call capture_want_luma;
// the next frame capture_poll uploads is also copied out. capture_copy_luma
// copies the latest snapshot (width*height bytes) and returns its sequence
// number, 0 if none yet.
void capture_want_luma(struct capture *c);
unsigned capture_copy_luma(struct capture *c, uint8_t *dst);

// Muting keeps capture running (so unmute is instant) but drops the audio.
void capture_set_muted(struct capture *c, int muted);
int capture_muted(struct capture *c);

#endif
