#include "capture.h"

#ifdef _WIN32
#include "capture_win.c"
#else

#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NBUF 4

struct vbuf { void *ptr; size_t len; };

struct capture {
  // video
  int fd;
  struct vbuf bufs[NBUF];
  unsigned nbufs, width, height, stride, fourcc;
  SDL_Texture *tex;
  // audio
  int have_audio;
  snd_pcm_t *pcm;
  SDL_AudioDeviceID out;
  unsigned max_bytes;
  pthread_t ath;
  atomic_int audio_running;
  atomic_int muted;
  // OCR snapshot
  atomic_int luma_wanted;
  pthread_mutex_t luma_lock;
  uint8_t *luma;
  unsigned luma_seq;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
  int r;
  do r = ioctl(fd, req, arg); while (r == -1 && errno == EINTR);
  return r;
}

/* ---------------- audio ---------------- */

static void *audio_thread(void *p)
{
  struct capture *c = p;
  int16_t buf[64 * 2];
  while (atomic_load(&c->audio_running)) {
    // Timed wait so shutdown never hangs when the source sends no audio.
    if (snd_pcm_wait(c->pcm, 100) == 0) continue;
    snd_pcm_sframes_t n = snd_pcm_readi(c->pcm, buf, 64);
    if (n == -EAGAIN) continue;
    if (n < 0) {
      if (snd_pcm_recover(c->pcm, (int)n, 1) < 0) break;
      continue;
    }
    if (atomic_load(&c->muted)) continue;
    // Drop input rather than let the output queue (and latency) grow.
    if (SDL_GetQueuedAudioSize(c->out) > c->max_bytes) continue;
    SDL_QueueAudio(c->out, buf, (Uint32)n * 4);
  }
  return NULL;
}

static int audio_start(struct capture *c, const struct capture_opts *o)
{
  int err;
  if ((err = snd_pcm_open(&c->pcm, o->audio_dev, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0) {
    fprintf(stderr, "audio: cannot open %s: %s\n", o->audio_dev, snd_strerror(err));
    return -1;
  }
  snd_pcm_hw_params_t *hw;
  snd_pcm_hw_params_alloca(&hw);
  snd_pcm_hw_params_any(c->pcm, hw);
  snd_pcm_hw_params_set_access(c->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  snd_pcm_hw_params_set_format(c->pcm, hw, SND_PCM_FORMAT_S16_LE);
  snd_pcm_hw_params_set_channels(c->pcm, hw, 2);
  unsigned rate = 48000;
  snd_pcm_hw_params_set_rate_near(c->pcm, hw, &rate, 0);
  snd_pcm_uframes_t period = 48, bufsz = 48 * 8;
  snd_pcm_hw_params_set_period_size_near(c->pcm, hw, &period, 0);
  snd_pcm_hw_params_set_buffer_size_near(c->pcm, hw, &bufsz);
  if ((err = snd_pcm_hw_params(c->pcm, hw)) < 0) {
    fprintf(stderr, "audio: hw params: %s\n", snd_strerror(err));
    snd_pcm_close(c->pcm);
    return -1;
  }

  SDL_AudioSpec want = {0}, have;
  want.freq = (int)rate;
  want.format = AUDIO_S16LSB;
  want.channels = 2;
  want.samples = 128;
  c->out = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!c->out) {
    fprintf(stderr, "audio: SDL output: %s\n", SDL_GetError());
    snd_pcm_close(c->pcm);
    return -1;
  }
  c->max_bytes = rate * 4 * (unsigned)o->audio_max_ms / 1000;
  fprintf(stderr, "audio: %s %u Hz, capture period %lu, output buffer %d samples\n",
          o->audio_dev, rate, (unsigned long)period, have.samples);
  SDL_PauseAudioDevice(c->out, 0);
  snd_pcm_start(c->pcm);
  atomic_store(&c->audio_running, 1);
  if (pthread_create(&c->ath, NULL, audio_thread, c) != 0) {
    atomic_store(&c->audio_running, 0);
    SDL_CloseAudioDevice(c->out);
    snd_pcm_close(c->pcm);
    return -1;
  }
  return 0;
}

/* ---------------- video ---------------- */

static int video_open(struct capture *c, const struct capture_opts *o)
{
  c->fd = open(o->video_dev, O_RDWR | O_NONBLOCK);
  if (c->fd < 0) { perror(o->video_dev); return -1; }

  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = (unsigned)o->width;
  fmt.fmt.pix.height = (unsigned)o->height;
  fmt.fmt.pix.pixelformat = o->fourcc;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) { perror("VIDIOC_S_FMT"); return -1; }
  if (fmt.fmt.pix.pixelformat != o->fourcc) {
    fprintf(stderr, "video: device refused requested pixel format\n");
    return -1;
  }
  c->width = fmt.fmt.pix.width;
  c->height = fmt.fmt.pix.height;
  c->stride = fmt.fmt.pix.bytesperline;
  c->fourcc = fmt.fmt.pix.pixelformat;

  struct v4l2_streamparm parm = {0};
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parm.parm.capture.timeperframe.numerator = 1;
  parm.parm.capture.timeperframe.denominator = (unsigned)o->fps;
  xioctl(c->fd, VIDIOC_S_PARM, &parm);
  struct v4l2_fract tpf = parm.parm.capture.timeperframe;

  struct v4l2_requestbuffers req = {0};
  req.count = NBUF;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); return -1; }
  c->nbufs = req.count < NBUF ? req.count : NBUF;
  for (unsigned i = 0; i < c->nbufs; i++) {
    struct v4l2_buffer b = {0};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) { perror("VIDIOC_QUERYBUF"); return -1; }
    c->bufs[i].len = b.length;
    c->bufs[i].ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, b.m.offset);
    if (c->bufs[i].ptr == MAP_FAILED) { perror("mmap"); return -1; }
    if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); return -1; }
  }
  enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0) { perror("VIDIOC_STREAMON"); return -1; }

  fprintf(stderr, "video: %s %ux%u %.4s @ %u/%u fps, %u buffers\n", o->video_dev,
          c->width, c->height, (char *)&c->fourcc, tpf.denominator, tpf.numerator, c->nbufs);
  return 0;
}

// Dequeue everything that is ready, keep only the newest buffer. Returns index or -1.
static int video_grab_latest(struct capture *c)
{
  int latest = -1;
  for (;;) {
    struct v4l2_buffer b = {0};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0) break;
    if (latest >= 0) {
      struct v4l2_buffer r = {.type = b.type, .memory = b.memory, .index = (unsigned)latest};
      xioctl(c->fd, VIDIOC_QBUF, &r);
    }
    latest = (b.flags & V4L2_BUF_FLAG_ERROR) ? (xioctl(c->fd, VIDIOC_QBUF, &b), -1) : (int)b.index;
  }
  return latest;
}

static void video_requeue(struct capture *c, int idx)
{
  struct v4l2_buffer b = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
                          .index = (unsigned)idx};
  xioctl(c->fd, VIDIOC_QBUF, &b);
}

/* ---------------- public ---------------- */

struct capture *capture_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  struct capture *c = calloc(1, sizeof *c);
  c->fd = -1;
  pthread_mutex_init(&c->luma_lock, NULL);
  if (video_open(c, o) < 0) { capture_close(c); return NULL; }

  Uint32 tex_fmt = c->fourcc == SV_FMT_NV12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_YUY2;
  c->tex = SDL_CreateTexture(ren, tex_fmt, SDL_TEXTUREACCESS_STREAMING, (int)c->width, (int)c->height);
  if (!c->tex) { fprintf(stderr, "SDL texture: %s\n", SDL_GetError()); capture_close(c); return NULL; }
  SDL_SetYUVConversionMode(c->height > 576 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601);

  c->have_audio = !o->no_audio && audio_start(c, o) == 0;
  return c;
}

void capture_close(struct capture *c)
{
  if (!c) return;
  if (c->have_audio) {
    atomic_store(&c->audio_running, 0);
    pthread_join(c->ath, NULL);
    SDL_CloseAudioDevice(c->out);
    snd_pcm_close(c->pcm);
  }
  if (c->fd >= 0) {
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(c->fd, VIDIOC_STREAMOFF, &t);
    for (unsigned i = 0; i < c->nbufs; i++)
      if (c->bufs[i].ptr) munmap(c->bufs[i].ptr, c->bufs[i].len);
    close(c->fd);
  }
  if (c->tex) SDL_DestroyTexture(c->tex);
  free(c->luma);
  pthread_mutex_destroy(&c->luma_lock);
  free(c);
}

int capture_poll(struct capture *c, int timeout_ms)
{
  struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
  if (poll(&pfd, 1, timeout_ms) <= 0) return 0;
  int idx = video_grab_latest(c);
  if (idx < 0) return 0;
  const Uint8 *p = c->bufs[idx].ptr;
  if (atomic_exchange(&c->luma_wanted, 0)) {
    pthread_mutex_lock(&c->luma_lock);
    if (!c->luma) c->luma = malloc((size_t)c->width * c->height);
    for (unsigned y = 0; y < c->height; y++) {
      const Uint8 *row = p + (size_t)y * c->stride;
      uint8_t *out = c->luma + (size_t)y * c->width;
      if (c->fourcc == SV_FMT_NV12) memcpy(out, row, c->width);
      else for (unsigned x = 0; x < c->width; x++) out[x] = row[2 * x];  // YUYV
    }
    c->luma_seq++;
    pthread_mutex_unlock(&c->luma_lock);
  }
  if (c->fourcc == SV_FMT_NV12)
    SDL_UpdateNVTexture(c->tex, NULL, p, (int)c->stride, p + c->stride * c->height, (int)c->stride);
  else
    SDL_UpdateTexture(c->tex, NULL, p, (int)c->stride);
  video_requeue(c, idx);
  return 1;
}

SDL_Texture *capture_texture(struct capture *c) { return c->tex; }
unsigned capture_width(struct capture *c) { return c->width; }
unsigned capture_height(struct capture *c) { return c->height; }
int capture_has_audio(struct capture *c) { return c->have_audio; }
double capture_audio_queued_ms(struct capture *c)
{
  return c->have_audio ? SDL_GetQueuedAudioSize(c->out) / 192.0 : 0.0;
}

void capture_set_muted(struct capture *c, int muted)
{
  atomic_store(&c->muted, muted);
  if (muted && c->have_audio) SDL_ClearQueuedAudio(c->out);
}

int capture_muted(struct capture *c) { return atomic_load(&c->muted); }

void capture_want_luma(struct capture *c) { atomic_store(&c->luma_wanted, 1); }

unsigned capture_copy_luma(struct capture *c, uint8_t *dst)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_seq;
  if (seq) memcpy(dst, c->luma, (size_t)c->width * c->height);
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

#endif // !_WIN32
