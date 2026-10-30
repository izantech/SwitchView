// SwitchView: Windows native video & audio capture backend.
// Video: Windows Media Foundation (IMFSourceReader) with synthetic test pattern fallback.
// Audio: SDL2 native WASAPI audio recording and low-latency playback.

#ifndef SV_CAPTURE_WIN_C
#define SV_CAPTURE_WIN_C

#include "capture.h"
#include "font8x8.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct capture {
  // Video - config & state
  int is_test;
  unsigned width, height, stride, fourcc;
  SDL_Texture *tex;
  atomic_int video_running;
  pthread_t vth;

  // Video - Media Foundation
  IMFSourceReader *reader;
  IMFMediaSource *source;

  // Video - Frame exchange (background thread -> capture_poll)
  pthread_mutex_t frame_lock;
  HANDLE frame_event;
  uint8_t *frame_buf;
  size_t frame_buf_size;
  unsigned frame_seq;
  unsigned last_poll_seq;
  int fps;

  // Audio (SDL2 WASAPI recording + playback)
  int have_audio;
  SDL_AudioDeviceID in;
  SDL_AudioDeviceID out;
  unsigned max_bytes;
  double bytes_per_ms;
  atomic_int audio_running;
  atomic_int muted;

  // OCR snapshot
  atomic_int luma_wanted;
  pthread_mutex_t luma_lock;
  uint8_t *luma;
  unsigned luma_seq;
};

/* ---------------- Audio (SDL2 WASAPI capture) ---------------- */

static void audio_capture_cb(void *userdata, Uint8 *stream, int len)
{
  struct capture *c = userdata;
  if (!atomic_load(&c->audio_running)) return;
  if (atomic_load(&c->muted)) return;

  // Bounded latency: drop frames if output queue exceeds maximum allowed size
  if (SDL_GetQueuedAudioSize(c->out) > c->max_bytes) return;

  SDL_QueueAudio(c->out, stream, (Uint32)len);
}

static int win_audio_start(struct capture *c, const struct capture_opts *o)
{
  SDL_AudioSpec want = {0}, have;
  want.freq = 48000;
  want.format = AUDIO_S16LSB;
  want.channels = 2;
  want.samples = 128; // ~2.6 ms buffer chunk for lowest latency
  want.callback = audio_capture_cb;
  want.userdata = c;

  const char *dev_name = o->audio_dev;
  if (dev_name && (!strcmp(dev_name, "default") || !strcmp(dev_name, "none") || !dev_name[0])) {
    dev_name = NULL;
  }

  c->in = SDL_OpenAudioDevice(dev_name, 1, &want, &have, 0);
  if (!c->in && dev_name != NULL) {
    fprintf(stderr, "audio: '%s' capture open failed: %s; trying default\n", dev_name, SDL_GetError());
    c->in = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
  }
  if (!c->in) {
    fprintf(stderr, "audio: capture device open failed: %s\n", SDL_GetError());
    return -1;
  }

  SDL_AudioSpec out_want = {0}, out_have;
  out_want.freq = have.freq;
  out_want.format = have.format;
  out_want.channels = have.channels;
  out_want.samples = 128;

  c->out = SDL_OpenAudioDevice(NULL, 0, &out_want, &out_have, 0);
  if (!c->out) {
    fprintf(stderr, "audio: output playback device open failed: %s\n", SDL_GetError());
    SDL_CloseAudioDevice(c->in);
    c->in = 0;
    return -1;
  }

  unsigned rate = (unsigned)have.freq;
  unsigned ch = (unsigned)have.channels;
  unsigned bytes_per_sec = rate * ch * sizeof(int16_t);
  c->bytes_per_ms = (double)bytes_per_sec / 1000.0;
  c->max_bytes = bytes_per_sec * (unsigned)o->audio_max_ms / 1000;
  if (c->max_bytes < (unsigned)have.samples * ch * sizeof(int16_t) * 2) {
    c->max_bytes = (unsigned)have.samples * ch * sizeof(int16_t) * 2;
  }

  fprintf(stderr, "audio: %u Hz %u ch, buffer %u samples, max queue %u ms (%u bytes)\n",
          rate, ch, have.samples, o->audio_max_ms, c->max_bytes);

  atomic_store(&c->audio_running, 1);
  SDL_PauseAudioDevice(c->out, 0);
  SDL_PauseAudioDevice(c->in, 0);
  return 0;
}

/* ---------------- Synthetic Test Pattern Generator ---------------- */

static const struct { uint8_t y, u, v; } kColorBars[8] = {
  { 235, 128, 128 }, // White (100%)
  { 210,  16, 146 }, // Yellow
  { 170, 166,  16 }, // Cyan
  { 145,  54,  34 }, // Green
  { 106, 202, 222 }, // Magenta
  {  81,  90, 240 }, // Red
  {  41, 240, 110 }, // Blue
  {  16, 128, 128 }  // Black
};

static void draw_glyph(uint8_t *y_plane, unsigned stride, unsigned w, unsigned h,
                       int x0, int y0, int scale, char ch, uint8_t color)
{
  unsigned char uch = (unsigned char)ch;
  if (uch < 32 || uch > 126) uch = '?';
  const uint8_t *glyph = sv_font8x8[uch - 32];
  for (int r = 0; r < 8; r++) {
    uint8_t bits = glyph[r];
    for (int c = 0; c < 8; c++) {
      if (bits & (0x80 >> c)) {
        for (int dy = 0; dy < scale; dy++) {
          int y = y0 + r * scale + dy;
          if (y < 0 || (unsigned)y >= h) continue;
          for (int dx = 0; dx < scale; dx++) {
            int x = x0 + c * scale + dx;
            if (x < 0 || (unsigned)x >= w) continue;
            y_plane[(size_t)y * stride + (size_t)x] = color;
          }
        }
      }
    }
  }
}

static void draw_text_str(uint8_t *y_plane, unsigned stride, unsigned w, unsigned h,
                          int x0, int y0, int scale, const char *str, uint8_t color)
{
  int cx = x0;
  for (const char *p = str; *p; p++) {
    draw_glyph(y_plane, stride, w, h, cx, y0, scale, *p, color);
    cx += 8 * scale;
  }
}

static void render_test_pattern(struct capture *c, Uint32 elapsed_ms)
{
  unsigned w = c->width, h = c->height, stride = c->stride;
  unsigned y_bars = h * 65 / 100;
  unsigned y_sweep = h * 73 / 100;

  if (c->fourcc == SV_FMT_NV12) {
    uint8_t *y_plane = c->frame_buf;
    uint8_t *uv_plane = c->frame_buf + (size_t)stride * h;

    // 1. Top Section: 8 SMPTE Color Bars
    for (unsigned y = 0; y < y_bars; y++) {
      for (unsigned x = 0; x < w; x++) {
        unsigned b = (x * 8) / w;
        if (b > 7) b = 7;
        y_plane[(size_t)y * stride + x] = kColorBars[b].y;
      }
    }
    for (unsigned uv_y = 0; uv_y < y_bars / 2; uv_y++) {
      for (unsigned x = 0; x < w; x += 2) {
        unsigned b = (x * 8) / w;
        if (b > 7) b = 7;
        uv_plane[(size_t)uv_y * stride + x] = kColorBars[b].u;
        uv_plane[(size_t)uv_y * stride + x + 1] = kColorBars[b].v;
      }
    }

    // 2. Middle Section: Animated Live Sweep Ticker
    int sweep_w = (int)(w / 10);
    int sweep_pos = (int)(((uint64_t)c->frame_seq * 10) % (w + sweep_w)) - sweep_w;

    for (unsigned y = y_bars; y < y_sweep; y++) {
      for (unsigned x = 0; x < w; x++) {
        int is_sweep = ((int)x >= sweep_pos && (int)x < sweep_pos + sweep_w);
        y_plane[(size_t)y * stride + x] = is_sweep ? 230 : 28;
      }
    }
    for (unsigned uv_y = y_bars / 2; uv_y < y_sweep / 2; uv_y++) {
      for (unsigned x = 0; x < w; x += 2) {
        uv_plane[(size_t)uv_y * stride + x] = 128;
        uv_plane[(size_t)uv_y * stride + x + 1] = 128;
      }
    }

    // 3. Lower Section: Info Badge with Live Clock and OCR Test Text
    for (unsigned y = y_sweep; y < h; y++) {
      for (unsigned x = 0; x < w; x++) {
        y_plane[(size_t)y * stride + x] = 20;
      }
    }
    for (unsigned uv_y = y_sweep / 2; uv_y < h / 2; uv_y++) {
      for (unsigned x = 0; x < w; x += 2) {
        uv_plane[(size_t)uv_y * stride + x] = 128;
        uv_plane[(size_t)uv_y * stride + x + 1] = 128;
      }
    }

    // Cyan accent divider line
    for (unsigned y = y_sweep; y < y_sweep + 4 && y < h; y++) {
      for (unsigned x = 0; x < w; x++) {
        y_plane[(size_t)y * stride + x] = 170;
      }
    }
    for (unsigned uv_y = y_sweep / 2; uv_y < (y_sweep + 4) / 2 && uv_y < h / 2; uv_y++) {
      for (unsigned x = 0; x < w; x += 2) {
        uv_plane[(size_t)uv_y * stride + x] = 166;
        uv_plane[(size_t)uv_y * stride + x + 1] = 16;
      }
    }

    // Text rendering into luma plane
    int scale = w >= 1920 ? 3 : (w >= 1280 ? 2 : 1);
    int line_h = 10 * scale;
    int tx = 32 * scale;
    int ty = (int)y_sweep + 12 * scale;

    draw_text_str(y_plane, stride, w, h, tx, ty, scale, "SWITCHVIEW TEST PATTERN", 245);
    ty += line_h;
    draw_text_str(y_plane, stride, w, h, tx, ty, scale, "NO HARDWARE DETECTED - SIMULATOR RUNNING", 220);
    ty += line_h;

    unsigned hh = (elapsed_ms / 3600000) % 24;
    unsigned mm = (elapsed_ms / 60000) % 60;
    unsigned ss = (elapsed_ms / 1000) % 60;
    unsigned ms = elapsed_ms % 1000;
    char timer_buf[128];
    snprintf(timer_buf, sizeof timer_buf, "TIME: %02u:%02u:%02u.%03u   FRAME: %06u   FPS: %u",
             hh, mm, ss, ms, c->frame_seq, c->fps);
    draw_text_str(y_plane, stride, w, h, tx, ty, scale, timer_buf, 245);
    ty += line_h;

    draw_text_str(y_plane, stride, w, h, tx, ty, scale, "STATUS: READY FOR INPUT AND OCR", 240);
    ty += line_h;
    draw_text_str(y_plane, stride, w, h, tx, ty, scale, "ABCDEFGHIJKLM NOPQRSTUVWXYZ 0123456789", 200);

  } else {
    // YUYV / YUY2 layout: 2 bytes per pixel (Y0 U Y1 V)
    uint8_t *p = c->frame_buf;
    for (unsigned y = 0; y < y_bars; y++) {
      for (unsigned x = 0; x < w; x += 2) {
        unsigned b = (x * 8) / w;
        if (b > 7) b = 7;
        size_t off = (size_t)y * stride + (size_t)x * 2;
        p[off + 0] = kColorBars[b].y;
        p[off + 1] = kColorBars[b].u;
        p[off + 2] = kColorBars[b].y;
        p[off + 3] = kColorBars[b].v;
      }
    }
    int sweep_w = (int)(w / 10);
    int sweep_pos = (int)(((uint64_t)c->frame_seq * 10) % (w + sweep_w)) - sweep_w;
    for (unsigned y = y_bars; y < y_sweep; y++) {
      for (unsigned x = 0; x < w; x += 2) {
        int is_sw0 = ((int)x >= sweep_pos && (int)x < sweep_pos + sweep_w);
        int is_sw1 = ((int)(x + 1) >= sweep_pos && (int)(x + 1) < sweep_pos + sweep_w);
        size_t off = (size_t)y * stride + (size_t)x * 2;
        p[off + 0] = is_sw0 ? 230 : 28;
        p[off + 1] = 128;
        p[off + 2] = is_sw1 ? 230 : 28;
        p[off + 3] = 128;
      }
    }
    for (unsigned y = y_sweep; y < h; y++) {
      for (unsigned x = 0; x < w; x += 2) {
        size_t off = (size_t)y * stride + (size_t)x * 2;
        p[off + 0] = 20;
        p[off + 1] = 128;
        p[off + 2] = 20;
        p[off + 3] = 128;
      }
    }
  }
}

static void *win_test_pattern_thread(void *arg)
{
  struct capture *c = arg;
  Uint32 start_ms = SDL_GetTicks();
  int target_fps = c->fps > 0 ? c->fps : 60;
  Uint32 interval = 1000 / target_fps;
  if (interval == 0) interval = 16;

  while (atomic_load(&c->video_running)) {
    Uint32 t0 = SDL_GetTicks();
    pthread_mutex_lock(&c->frame_lock);
    render_test_pattern(c, t0 - start_ms);
    c->frame_seq++;
    SetEvent(c->frame_event);
    pthread_mutex_unlock(&c->frame_lock);

    Uint32 elapsed = SDL_GetTicks() - t0;
    if (elapsed < interval) {
      SDL_Delay(interval - elapsed);
    }
  }
  return NULL;
}

/* ---------------- Video (Media Foundation IMFSourceReader) ---------------- */

static int strcasestr_compat(const char *haystack, const char *needle)
{
  if (!haystack || !needle) return 0;
  size_t nlen = strlen(needle);
  if (nlen == 0) return 1;
  for (; *haystack; haystack++) {
    if (_strnicmp(haystack, needle, nlen) == 0) return 1;
  }
  return 0;
}

static void *win_mf_video_thread(void *arg)
{
  struct capture *c = arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  while (atomic_load(&c->video_running)) {
    DWORD streamFlags = 0;
    LONGLONG timestamp = 0;
    IMFSample *pSample = NULL;
    DWORD streamIndex = 0;

    HRESULT hr = c->reader->lpVtbl->ReadSample(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                               0, &streamIndex, &streamFlags, &timestamp, &pSample);
    if (FAILED(hr) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      if (pSample) pSample->lpVtbl->Release(pSample);
      SDL_Delay(4);
      continue;
    }
    if (!pSample) continue;

    IMFMediaBuffer *pBuffer = NULL;
    hr = pSample->lpVtbl->ConvertToContiguousBuffer(pSample, &pBuffer);
    if (SUCCEEDED(hr) && pBuffer) {
      BYTE *data = NULL;
      DWORD maxLen = 0, curLen = 0;
      hr = pBuffer->lpVtbl->Lock(pBuffer, &data, &maxLen, &curLen);
      if (SUCCEEDED(hr) && data && curLen > 0) {
        pthread_mutex_lock(&c->frame_lock);
        size_t to_copy = curLen < c->frame_buf_size ? curLen : c->frame_buf_size;
        memcpy(c->frame_buf, data, to_copy);
        c->frame_seq++;
        SetEvent(c->frame_event);
        pthread_mutex_unlock(&c->frame_lock);
        pBuffer->lpVtbl->Unlock(pBuffer);
      }
      pBuffer->lpVtbl->Release(pBuffer);
    }
    pSample->lpVtbl->Release(pSample);
  }

  CoUninitialize();
  return NULL;
}

static int win_video_open(struct capture *c, const struct capture_opts *o)
{
  HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return -1;
  hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (FAILED(hr)) return -1;

  IMFAttributes *attr = NULL;
  hr = MFCreateAttributes(&attr, 1);
  if (FAILED(hr)) return -1;
  hr = attr->lpVtbl->SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                             &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) { attr->lpVtbl->Release(attr); return -1; }

  IMFActivate **devs = NULL;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attr, &devs, &count);
  attr->lpVtbl->Release(attr);
  if (FAILED(hr) || count == 0) {
    fprintf(stderr, "video: no video capture hardware devices found\n");
    return -1;
  }

  UINT32 selected = 0;
  if (o->video_dev && o->video_dev[0]) {
    char *endptr = NULL;
    long idx = strtol(o->video_dev, &endptr, 10);
    if (endptr && *endptr == '\0' && idx >= 0 && (UINT32)idx < count) {
      selected = (UINT32)idx;
    } else {
      int found = 0;
      for (UINT32 i = 0; i < count; i++) {
        WCHAR wname[256] = {0};
        UINT32 len = 0;
        if (SUCCEEDED(devs[i]->lpVtbl->GetString(devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                 wname, 256, &len))) {
          char cname[256] = {0};
          WideCharToMultiByte(CP_UTF8, 0, wname, -1, cname, sizeof cname, NULL, NULL);
          if (strcasestr_compat(cname, o->video_dev)) {
            selected = i;
            found = 1;
            break;
          }
        }
      }
      if (!found) {
        fprintf(stderr, "video: device '%s' not found, defaulting to index 0\n", o->video_dev);
        selected = 0;
      }
    }
  }

  WCHAR wfriendly[256] = {0};
  UINT32 nameLen = 0;
  devs[selected]->lpVtbl->GetString(devs[selected], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                    wfriendly, 256, &nameLen);
  char friendly[256] = {0};
  WideCharToMultiByte(CP_UTF8, 0, wfriendly, -1, friendly, sizeof friendly, NULL, NULL);

  hr = devs[selected]->lpVtbl->ActivateObject(devs[selected], &IID_IMFMediaSource, (void **)&c->source);
  for (UINT32 i = 0; i < count; i++) devs[i]->lpVtbl->Release(devs[i]);
  CoTaskMemFree(devs);
  if (FAILED(hr) || !c->source) {
    fprintf(stderr, "video: failed to activate device '%s' (0x%08lx)\n", friendly, hr);
    return -1;
  }

  IMFAttributes *ra = NULL;
  MFCreateAttributes(&ra, 1);
  if (ra) ra->lpVtbl->SetUINT32(ra, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  hr = MFCreateSourceReaderFromMediaSource(c->source, ra, &c->reader);
  if (ra) ra->lpVtbl->Release(ra);
  if (FAILED(hr) || !c->reader) {
    fprintf(stderr, "video: MFCreateSourceReader failed (0x%08lx)\n", hr);
    c->source->lpVtbl->Shutdown(c->source);
    c->source->lpVtbl->Release(c->source);
    c->source = NULL;
    return -1;
  }

  // Set reader media type
  GUID targetSubtype = (o->fourcc == SV_FMT_YUYV) ? MFVideoFormat_YUY2 : MFVideoFormat_NV12;
  IMFMediaType *partial = NULL;
  MFCreateMediaType(&partial);
  partial->lpVtbl->SetGUID(partial, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
  partial->lpVtbl->SetGUID(partial, &MF_MT_SUBTYPE, &targetSubtype);
  UINT64 frame_size = ((UINT64)o->width << 32) | (UINT32)o->height;
  partial->lpVtbl->SetUINT64(partial, &MF_MT_FRAME_SIZE, frame_size);
  if (o->fps > 0) {
    UINT64 fps_ratio = ((UINT64)o->fps << 32) | 1;
    partial->lpVtbl->SetUINT64(partial, &MF_MT_FRAME_RATE, fps_ratio);
  }

  hr = c->reader->lpVtbl->SetCurrentMediaType(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, partial);
  if (FAILED(hr)) {
    partial->lpVtbl->DeleteItem(partial, &MF_MT_FRAME_RATE);
    hr = c->reader->lpVtbl->SetCurrentMediaType(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, partial);
  }
  if (FAILED(hr)) {
    partial->lpVtbl->DeleteItem(partial, &MF_MT_FRAME_SIZE);
    hr = c->reader->lpVtbl->SetCurrentMediaType(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, partial);
  }
  if (FAILED(hr)) {
    GUID altSub = IsEqualGUID(&targetSubtype, &MFVideoFormat_NV12) ? MFVideoFormat_YUY2 : MFVideoFormat_NV12;
    partial->lpVtbl->SetGUID(partial, &MF_MT_SUBTYPE, &altSub);
    hr = c->reader->lpVtbl->SetCurrentMediaType(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, partial);
  }
  partial->lpVtbl->Release(partial);

  if (FAILED(hr)) {
    fprintf(stderr, "video: failed to set media type on '%s' (0x%08lx)\n", friendly, hr);
    c->reader->lpVtbl->Release(c->reader);
    c->reader = NULL;
    c->source->lpVtbl->Shutdown(c->source);
    c->source->lpVtbl->Release(c->source);
    c->source = NULL;
    return -1;
  }

  IMFMediaType *cur = NULL;
  hr = c->reader->lpVtbl->GetCurrentMediaType(c->reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
  if (SUCCEEDED(hr) && cur) {
    UINT64 actualSize = 0;
    if (SUCCEEDED(cur->lpVtbl->GetUINT64(cur, &MF_MT_FRAME_SIZE, &actualSize))) {
      c->width = (unsigned)(actualSize >> 32);
      c->height = (unsigned)(actualSize & 0xFFFFFFFF);
    }
    GUID sub = {0};
    cur->lpVtbl->GetGUID(cur, &MF_MT_SUBTYPE, &sub);
    c->fourcc = IsEqualGUID(&sub, &MFVideoFormat_YUY2) ? SV_FMT_YUYV : SV_FMT_NV12;

    UINT32 str = 0;
    if (SUCCEEDED(cur->lpVtbl->GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &str))) {
      c->stride = str;
    } else {
      c->stride = (c->fourcc == SV_FMT_NV12) ? c->width : (c->width * 2);
    }
    cur->lpVtbl->Release(cur);
  } else {
    c->width = o->width;
    c->height = o->height;
    c->fourcc = o->fourcc;
    c->stride = (c->fourcc == SV_FMT_NV12) ? c->width : (c->width * 2);
  }

  c->fps = o->fps > 0 ? o->fps : 60;
  fprintf(stderr, "video: %s %ux%u %.4s @ %d fps\n", friendly, c->width, c->height,
          (const char *)&c->fourcc, c->fps);
  return 0;
}

/* ---------------- Public API ---------------- */

struct capture *capture_open(const struct capture_opts *o, SDL_Renderer *ren)
{
  struct capture *c = calloc(1, sizeof *c);
  if (!c) return NULL;

  pthread_mutex_init(&c->frame_lock, NULL);
  pthread_mutex_init(&c->luma_lock, NULL);
  c->frame_event = CreateEvent(NULL, FALSE, FALSE, NULL);

  int force_test = (o->video_dev && (!strcmp(o->video_dev, "test") || !strcmp(o->video_dev, "none")));
  if (force_test || win_video_open(c, o) < 0) {
    c->is_test = 1;
    c->width = o->width > 0 ? (unsigned)o->width : 1920;
    c->height = o->height > 0 ? (unsigned)o->height : 1080;
    c->fps = o->fps > 0 ? o->fps : 60;
    c->fourcc = (o->fourcc == SV_FMT_YUYV) ? SV_FMT_YUYV : SV_FMT_NV12;
    c->stride = (c->fourcc == SV_FMT_NV12) ? c->width : (c->width * 2);
    fprintf(stderr, "video: active backend = Synthetic Test Pattern (%ux%u %.4s @ %d fps)\n",
            c->width, c->height, (const char *)&c->fourcc, c->fps);
  }

  size_t buf_size = (c->fourcc == SV_FMT_NV12)
      ? (size_t)c->stride * c->height + (size_t)c->stride * (c->height / 2)
      : (size_t)c->stride * c->height;
  c->frame_buf = malloc(buf_size);
  c->frame_buf_size = buf_size;
  if (!c->frame_buf) {
    capture_close(c);
    return NULL;
  }
  memset(c->frame_buf, 0, buf_size);

  Uint32 tex_fmt = (c->fourcc == SV_FMT_NV12) ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_YUY2;
  c->tex = SDL_CreateTexture(ren, tex_fmt, SDL_TEXTUREACCESS_STREAMING, (int)c->width, (int)c->height);
  if (!c->tex) {
    fprintf(stderr, "SDL texture: %s\n", SDL_GetError());
    capture_close(c);
    return NULL;
  }
  SDL_SetYUVConversionMode(c->height > 576 ? SDL_YUV_CONVERSION_BT709 : SDL_YUV_CONVERSION_BT601);

  atomic_store(&c->video_running, 1);
  if (c->is_test) {
    pthread_create(&c->vth, NULL, win_test_pattern_thread, c);
  } else {
    pthread_create(&c->vth, NULL, win_mf_video_thread, c);
  }

  c->have_audio = !o->no_audio && win_audio_start(c, o) == 0;
  return c;
}

void capture_close(struct capture *c)
{
  if (!c) return;

  if (c->have_audio) {
    atomic_store(&c->audio_running, 0);
    if (c->in) {
      SDL_PauseAudioDevice(c->in, 1);
      SDL_CloseAudioDevice(c->in);
      c->in = 0;
    }
    if (c->out) {
      SDL_PauseAudioDevice(c->out, 1);
      SDL_CloseAudioDevice(c->out);
      c->out = 0;
    }
  }

  if (atomic_load(&c->video_running)) {
    atomic_store(&c->video_running, 0);
    SetEvent(c->frame_event);
    pthread_join(c->vth, NULL);
  }

  if (c->reader) {
    c->reader->lpVtbl->Release(c->reader);
    c->reader = NULL;
  }
  if (c->source) {
    c->source->lpVtbl->Shutdown(c->source);
    c->source->lpVtbl->Release(c->source);
    c->source = NULL;
  }
  if (!c->is_test) {
    MFShutdown();
    CoUninitialize();
  }

  if (c->frame_event) CloseHandle(c->frame_event);
  if (c->tex) SDL_DestroyTexture(c->tex);
  free(c->frame_buf);
  free(c->luma);
  pthread_mutex_destroy(&c->frame_lock);
  pthread_mutex_destroy(&c->luma_lock);
  free(c);
}

int capture_poll(struct capture *c, int timeout_ms)
{
  DWORD wait_ms = timeout_ms > 0 ? (DWORD)timeout_ms : 0;
  DWORD wr = WaitForSingleObject(c->frame_event, wait_ms);
  if (wr != WAIT_OBJECT_0 && c->frame_seq == c->last_poll_seq) {
    return 0;
  }

  pthread_mutex_lock(&c->frame_lock);
  if (c->frame_seq == c->last_poll_seq) {
    pthread_mutex_unlock(&c->frame_lock);
    return 0;
  }
  c->last_poll_seq = c->frame_seq;
  const uint8_t *p = c->frame_buf;

  // Grayscale (luma) snapshot for OCR if requested
  if (atomic_exchange(&c->luma_wanted, 0)) {
    pthread_mutex_lock(&c->luma_lock);
    if (!c->luma) c->luma = malloc((size_t)c->width * c->height);
    if (c->fourcc == SV_FMT_NV12) {
      for (unsigned y = 0; y < c->height; y++) {
        memcpy(c->luma + (size_t)y * c->width, p + (size_t)y * c->stride, c->width);
      }
    } else {
      for (unsigned y = 0; y < c->height; y++) {
        const uint8_t *row = p + (size_t)y * c->stride;
        uint8_t *out = c->luma + (size_t)y * c->width;
        for (unsigned x = 0; x < c->width; x++) out[x] = row[2 * x];
      }
    }
    c->luma_seq++;
    pthread_mutex_unlock(&c->luma_lock);
  }

  if (c->fourcc == SV_FMT_NV12) {
    SDL_UpdateNVTexture(c->tex, NULL, p, (int)c->stride,
                        p + (size_t)c->stride * c->height, (int)c->stride);
  } else {
    SDL_UpdateTexture(c->tex, NULL, p, (int)c->stride);
  }

  pthread_mutex_unlock(&c->frame_lock);
  return 1;
}

SDL_Texture *capture_texture(struct capture *c) { return c->tex; }
unsigned capture_width(struct capture *c) { return c->width; }
unsigned capture_height(struct capture *c) { return c->height; }
int capture_has_audio(struct capture *c) { return c->have_audio; }

double capture_audio_queued_ms(struct capture *c)
{
  if (!c->have_audio || !c->out || c->bytes_per_ms <= 0.0) return 0.0;
  return (double)SDL_GetQueuedAudioSize(c->out) / c->bytes_per_ms;
}

void capture_set_muted(struct capture *c, int muted)
{
  atomic_store(&c->muted, muted);
  if (muted && c->have_audio && c->out) {
    SDL_ClearQueuedAudio(c->out);
  }
}

int capture_muted(struct capture *c) { return atomic_load(&c->muted); }

void capture_want_luma(struct capture *c) { atomic_store(&c->luma_wanted, 1); }

unsigned capture_copy_luma(struct capture *c, uint8_t *dst)
{
  pthread_mutex_lock(&c->luma_lock);
  unsigned seq = c->luma_seq;
  if (seq && c->luma) {
    memcpy(dst, c->luma, (size_t)c->width * c->height);
  }
  pthread_mutex_unlock(&c->luma_lock);
  return seq;
}

#endif // SV_CAPTURE_WIN_C
