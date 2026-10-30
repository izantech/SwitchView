// SwitchView: low-latency Elgato capture viewer + Pro Controller / keyboard
// command panel driving a Pico 2 W that is plugged into the Switch 2 dock.
#include <SDL2/SDL.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("compat/platform.h")
#include "compat/platform.h"
#else
#include "platform.h"
#endif

#if __has_include("compat/dirent.h")
#include "compat/dirent.h"
#else
#include "dirent.h"
#endif

#include <getopt.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "capture.h"
#include "link.h"
#include "ocr.h"
#include "panel.h"
#include "script.h"
#include "switchview_proto.h"

static atomic_int running = 1;
static void on_signal(int sig) { (void)sig; atomic_store(&running, 0); }

static void usage(const char *argv0)
{
  fprintf(stderr,
          "usage: %s [options]\n"
          "  -d DEV     video device (default /dev/video0 or test)\n"
          "  -a PCM     audio capture device (default hw:CARD=S,DEV=0 or default)\n"
          "  -s WxH     capture size (default 1920x1080)\n"
          "  -r FPS     frame rate (default 60)\n"
          "  -p FMT     pixel format: nv12 | yuyv (default nv12)\n"
          "  -l MS      max audio queue in ms before dropping (default 20)\n"
          "  -H HOST    Pico address (default 192.168.4.60); 'none' disables the link\n"
          "  -P PORT    Pico UDP port (default %d)\n"
          "  -v         enable vsync (no tearing, up to +1 frame latency)\n"
          "  -f         start fullscreen (video only)\n"
          "  -n         no audio\n"
          "  -m SCALE   mouse sensitivity sent to the Switch (default 1.0)\n"
          "  -V         print stats every second\n"
          "keys: F11 fullscreen (hides panel), Ctrl+Q quit, Enter focus text box, Esc back to pad\n"
          "mouse: click the video to send the mouse to the Switch; Esc gives it back\n"
          "pad:  WASD left stick, arrows right stick, TFGH d-pad, IJKL = X Y B A,\n"
          "      Q/E = L/R, 1/3 = ZL/ZR, Z/C = L3/R3, -/= minus/plus, P home, M capture\n"
          "mouse: left = hold, right = latch (toggle)\n",
          argv0, SV_PORT);
}

int main(int argc, char **argv)
{
  sv_net_init();

#ifdef _WIN32
  struct capture_opts co = {.video_dev = "test", .audio_dev = "default",
                            .width = 1920, .height = 1080, .fps = 60,
                            .fourcc = V4L2_PIX_FMT_NV12, .audio_max_ms = 20};
#else
  struct capture_opts co = {.video_dev = "/dev/video0", .audio_dev = "hw:CARD=S,DEV=0",
                            .width = 1920, .height = 1080, .fps = 60,
                            .fourcc = V4L2_PIX_FMT_NV12, .audio_max_ms = 20};
#endif
  const char *host = "192.168.4.60";
  int port = SV_PORT, vsync = 0, fullscreen = 0, verbose = 0;
  float mouse_scale = 1.0f;
  int c;
  while ((c = getopt(argc, argv, "d:a:s:r:p:l:H:P:m:vfnVh")) != -1) {
    switch (c) {
    case 'd': co.video_dev = optarg; break;
    case 'a': co.audio_dev = optarg; break;
    case 's': if (sscanf(optarg, "%dx%d", &co.width, &co.height) != 2) { usage(argv[0]); sv_net_cleanup(); return 1; } break;
    case 'r': co.fps = atoi(optarg); break;
    case 'p': co.fourcc = !strcmp(optarg, "yuyv") ? V4L2_PIX_FMT_YUYV : V4L2_PIX_FMT_NV12; break;
    case 'l': co.audio_max_ms = atoi(optarg); break;
    case 'H': host = optarg; break;
    case 'P': port = atoi(optarg); break;
    case 'm': mouse_scale = (float)atof(optarg); break;
    case 'v': vsync = 1; break;
    case 'f': fullscreen = 1; break;
    case 'n': co.no_audio = 1; break;
    case 'V': verbose = 1; break;
    default: usage(argv[0]); sv_net_cleanup(); return c != 'h';
    }
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_APP_NAME, "SwitchView");
  SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "SwitchView");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    sv_net_cleanup();
    return 1;
  }

  SDL_Window *win = SDL_CreateWindow("SwitchView", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     960, 540 + 960 * PANEL_DESIGN_H / PANEL_DESIGN_W, SDL_WINDOW_ALLOW_HIGHDPI |
                                     (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                         (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
  if (!win || !ren) {
    fprintf(stderr, "SDL window/renderer: %s\n", SDL_GetError());
    SDL_Quit();
    sv_net_cleanup();
    return 1;
  }
  SDL_RendererInfo ri;
  SDL_GetRendererInfo(ren, &ri);
  fprintf(stderr, "render: %s, vsync %s\n", ri.name, vsync ? "on" : "off");

  struct capture *cap = capture_open(&co, ren);
  if (!cap) {
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    sv_net_cleanup();
    return 1;
  }
  struct link *link = strcmp(host, "none") ? link_open(host, port) : NULL;
  struct panel *panel = panel_create(ren);

  // Scripts live in SwitchView/scripts, next to app/ where the binary is.
  char script_dir[1200] = "scripts";
  char exe_dir[1024];
  if (sv_get_executable_dir(exe_dir, sizeof(exe_dir) - 32) == 0) {
    char test_dir[1200];
    snprintf(test_dir, sizeof(test_dir), "%s/scripts", exe_dir);
    DIR *d = opendir(test_dir);
    if (d) {
      closedir(d);
      snprintf(script_dir, sizeof(script_dir), "%s", test_dir);
    } else {
      snprintf(test_dir, sizeof(test_dir), "%s/../scripts", exe_dir);
      d = opendir(test_dir);
      if (d) {
        closedir(d);
        snprintf(script_dir, sizeof(script_dir), "%s", test_dir);
      }
    }
  }
  struct ocr *ocr = ocr_create(cap);
  struct script *script = script_create(ocr, script_dir);
  panel_set_script(panel, script);
  fprintf(stderr, "link: %s\n", link ? link_describe(link) : "disabled");

  unsigned frames = 0;
  double fps = 0;
  Uint32 t_last = SDL_GetTicks();
  int need_redraw = 1;
  char status[160] = "";
  int mouse_captured = 0;
  int mute_hover = 0, mouse_in_video = 0;
  float mute_alpha = 0;
  Uint32 last_frame_ms = SDL_GetTicks();
  unsigned mouse_btn = 0;
  float mouse_acc_x = 0, mouse_acc_y = 0;

  while (atomic_load(&running)) {
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    int panel_h = fullscreen ? 0 : (int)(ww * (float)PANEL_DESIGN_H / PANEL_DESIGN_W);
    SDL_Rect video_area = {0, 0, ww, wh - panel_h};
    SDL_Rect panel_area = {0, wh - panel_h, ww, panel_h};
    float mute_d = 40.f * ww / 960.f, mute_m = 14.f * ww / 960.f;
    SDL_FRect mute_rc = {mute_m, mute_m, mute_d, mute_d};

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) atomic_store(&running, 0);
      else if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_CTRL) && e.key.keysym.sym == SDLK_q)
        atomic_store(&running, 0);
      else if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F11) {
        fullscreen = !fullscreen;
        SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
      } else if (e.type == SDL_WINDOWEVENT) {
        need_redraw = 1;
        if (e.window.event == SDL_WINDOWEVENT_LEAVE) { mouse_in_video = 0; mute_hover = 0; }
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && mouse_captured) {
          mouse_captured = 0;
          SDL_SetRelativeMouseMode(SDL_FALSE);
          if (link) link_mouse(link, 0, 0, 0, mouse_btn = 0);
        }
      } else if (mouse_captured) {
        // The Switch owns the mouse: forward motion, buttons and wheel.
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
          mouse_captured = 0;
          SDL_SetRelativeMouseMode(SDL_FALSE);
          if (link) link_mouse(link, 0, 0, 0, mouse_btn = 0);
          need_redraw = 1;
        } else if (e.type == SDL_MOUSEMOTION) {
          mouse_acc_x += e.motion.xrel * mouse_scale;
          mouse_acc_y += e.motion.yrel * mouse_scale;
          int dx = (int)mouse_acc_x, dy = (int)mouse_acc_y;
          mouse_acc_x -= dx; mouse_acc_y -= dy;
          if (link && (dx || dy)) link_mouse(link, dx, dy, 0, mouse_btn);
        } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
          unsigned bit = e.button.button == SDL_BUTTON_LEFT ? SV_MOUSE_LEFT :
                         e.button.button == SDL_BUTTON_RIGHT ? SV_MOUSE_RIGHT :
                         e.button.button == SDL_BUTTON_MIDDLE ? SV_MOUSE_MIDDLE : 0;
          if (e.type == SDL_MOUSEBUTTONDOWN) mouse_btn |= bit; else mouse_btn &= ~bit;
          if (link) link_mouse(link, 0, 0, 0, mouse_btn);
        } else if (e.type == SDL_MOUSEWHEEL) {
          if (link) link_mouse(link, 0, 0, e.wheel.y, mouse_btn);
        } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
          if (panel_event(panel, &e, &panel_area)) need_redraw = 1;
        }
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
                 SDL_PointInFRect(&(SDL_FPoint){e.button.x, e.button.y}, &mute_rc)) {
        capture_set_muted(cap, !capture_muted(cap));
        need_redraw = 1;
      } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT &&
                 e.button.y < video_area.h && link) {
        // Click on the video: hand the mouse to the Switch.
        mouse_captured = 1;
        SDL_SetRelativeMouseMode(SDL_TRUE);
        need_redraw = 1;
      } else if (e.type == SDL_MOUSEMOTION && e.motion.y < video_area.h) {
        mouse_in_video = 1;
        int h = SDL_PointInFRect(&(SDL_FPoint){e.motion.x, e.motion.y}, &mute_rc);
        if (h != mute_hover) { mute_hover = h; need_redraw = 1; }
        if (panel_event(panel, &e, &panel_area)) need_redraw = 1;
      } else if (!fullscreen || e.type == SDL_KEYDOWN || e.type == SDL_KEYUP ||
               e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (e.type == SDL_MOUSEMOTION) { mouse_in_video = 0; mute_hover = 0; }
        if (panel_event(panel, &e, &panel_area)) need_redraw = 1;
      }
    }

    // Short poll keeps input responsive while waking the instant a frame lands.
    if (capture_poll(cap, 4)) { frames++; need_redraw = 1; }

    Uint32 now = SDL_GetTicks();
    if (link) {
      struct link_state st, ss;
      panel_get_state(panel, &st);
      // A running script drives the same pad: its buttons add to the
      // panel's, its sticks/d-pad win whenever it has them off-centre.
      int scripting;
      script_get_input(script, &ss, &scripting);
      if (scripting) {
        st.buttons |= ss.buttons;
        if (ss.lx || ss.ly) { st.lx = ss.lx; st.ly = ss.ly; }
        if (ss.rx || ss.ry) { st.rx = ss.rx; st.ry = ss.ry; }
        if (ss.hat != SV_HAT_CENTER) st.hat = ss.hat;
      }
      link_set_state(link, &st);
      char text[1024];
      if (panel_take_text(panel, text, sizeof text)) link_send_text(link, text);
      if (script_take_text(script, text, sizeof text)) link_send_text(link, text);
      link_poll(link, now);
      struct link_stats ls;
      link_get_stats(link, &ls);
      panel_set_link(panel, ls.up);
      script_set_leds(script, ls.up ? ls.leds : -1);
    }

    // Panel highlights follow held keys/gamepad even without events.
    if (!fullscreen && (now / 33) != (t_last / 33)) need_redraw = 1;

    {
      // Mute button fades in while the pointer is over the video.
      float target = mouse_in_video && !mouse_captured ? 1.f : 0.f;
      float dt = (now - last_frame_ms) / 1000.f;
      last_frame_ms = now;
      float na = mute_alpha + (target > mute_alpha ? 1 : -1) * dt / 0.18f;
      na = target > mute_alpha ? (na > target ? target : na) : (na < target ? target : na);
      if (na != mute_alpha) { mute_alpha = na; need_redraw = 1; }
    }
    if (need_redraw) {
      SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
      SDL_RenderClear(ren);
      // Aspect-fit the video into its area.
      float ar = (float)capture_width(cap) / capture_height(cap);
      SDL_Rect dst = video_area;
      if (dst.w / ar > dst.h) { dst.w = (int)(dst.h * ar); dst.x = (video_area.w - dst.w) / 2; }
      else { dst.h = (int)(dst.w / ar); dst.y = (video_area.h - dst.h) / 2; }
      SDL_RenderCopy(ren, capture_texture(cap), NULL, &dst);
      if (mouse_captured) {
        // Thin accent frame: the Switch has the mouse; Esc returns it.
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 52, 110, 230, 200);
        for (int i = 0; i < 2; i++) {
          SDL_Rect f = {dst.x + i, dst.y + i, dst.w - 2 * i, dst.h - 2 * i};
          SDL_RenderDrawRect(ren, &f);
        }
      }
      panel_draw_mute(panel, mute_rc, capture_muted(cap), mute_hover, mute_alpha);
      if (!fullscreen) panel_render(panel, &panel_area, status);
      SDL_RenderPresent(ren);
      need_redraw = 0;
    }

    if (now - t_last >= 1000) {
      fps = frames * 1000.0 / (now - t_last);
      struct link_stats ls = {0};
      if (link) link_get_stats(link, &ls);
      snprintf(status, sizeof status, "%ux%u %.0f fps   audio %s %.0f ms   link %s  %lu pkts%s%s",
               capture_width(cap), capture_height(cap), fps,
               capture_has_audio(cap) ? "q" : "off", capture_audio_queued_ms(cap),
               link ? link_describe(link) : "off", ls.packets,
               ls.send_errors ? "  SEND ERRORS" : "", ls.text_queued ? "  typing..." : "");
      if (verbose) fprintf(stderr, "%s\n", status);
      frames = 0;
      t_last = now;
    }
  }

  script_destroy(script);
  ocr_destroy(ocr);
  link_close(link);
  panel_destroy(panel);
  capture_close(cap);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  sv_net_cleanup();
  return 0;
}
