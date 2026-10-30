#include "panel.h"
#include "switchview_proto.h"
#include "script.h"

#if __has_include("compat/platform.h")
#include "compat/platform.h"
#else
#include "platform.h"
#endif

#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ---------------- layout ---------------- */

enum shape { SH_RECT, SH_CIRCLE };

// Hat directions are kept as pseudo-buttons above the real SV_BTN_* bits.
enum { DP_UP = 1u << 24, DP_DOWN = 1u << 25, DP_LEFT = 1u << 26, DP_RIGHT = 1u << 27 };
#define BTN_MASK 0x00ffffffu

struct widget {
  const char *label, *key;
  uint32_t bit;
  enum shape shape;
  int x, y, w, h;  // centre + size in design space
  SDL_Keycode keycode;
  SDL_GameControllerButton pad;
};

static const struct widget WIDGETS[] = {
  {"ZL", "1", SV_BTN_ZL, SH_RECT, 100, 30, 150, 34, SDLK_1, SDL_CONTROLLER_BUTTON_INVALID},
  {"L", "Q", SV_BTN_L, SH_RECT, 100, 72, 150, 34, SDLK_q, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
  {"ZR", "3", SV_BTN_ZR, SH_RECT, 620, 30, 150, 34, SDLK_3, SDL_CONTROLLER_BUTTON_INVALID},
  {"R", "E", SV_BTN_R, SH_RECT, 620, 72, 150, 34, SDLK_e, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
  {"\u2212", "-", SV_BTN_MINUS, SH_CIRCLE, 276, 128, 40, 40, SDLK_MINUS, SDL_CONTROLLER_BUTTON_BACK},
  {"+", "=", SV_BTN_PLUS, SH_CIRCLE, 444, 128, 40, 40, SDLK_EQUALS, SDL_CONTROLLER_BUTTON_START},
  {"", "M", SV_BTN_CAPTURE, SH_RECT, 318, 188, 40, 40, SDLK_m, SDL_CONTROLLER_BUTTON_MISC1},
  {"", "P", SV_BTN_HOME, SH_CIRCLE, 402, 188, 40, 40, SDLK_p, SDL_CONTROLLER_BUTTON_GUIDE},
  {"X", "I", SV_BTN_X, SH_CIRCLE, 620, 132, 52, 52, SDLK_i, SDL_CONTROLLER_BUTTON_Y},
  {"Y", "J", SV_BTN_Y, SH_CIRCLE, 564, 188, 52, 52, SDLK_j, SDL_CONTROLLER_BUTTON_X},
  {"A", "L", SV_BTN_A, SH_CIRCLE, 676, 188, 52, 52, SDLK_l, SDL_CONTROLLER_BUTTON_B},
  {"B", "K", SV_BTN_B, SH_CIRCLE, 620, 244, 52, 52, SDLK_k, SDL_CONTROLLER_BUTTON_A},
  {"\u25b2", "T", DP_UP, SH_RECT, 230, 238, 42, 42, SDLK_t, SDL_CONTROLLER_BUTTON_DPAD_UP},
  {"\u25bc", "G", DP_DOWN, SH_RECT, 230, 338, 42, 42, SDLK_g, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
  {"\u25c0", "F", DP_LEFT, SH_RECT, 180, 288, 42, 42, SDLK_f, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
  {"\u25b6", "H", DP_RIGHT, SH_RECT, 280, 288, 42, 42, SDLK_h, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
  {"L3", "Z", SV_BTN_L3, SH_RECT, 0, 0, 0, 0, SDLK_z, SDL_CONTROLLER_BUTTON_LEFTSTICK},
  {"R3", "C", SV_BTN_R3, SH_RECT, 0, 0, 0, 0, SDLK_c, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
};
#define NWIDGETS (sizeof WIDGETS / sizeof WIDGETS[0])

struct stick_layout { int x, y, r; SDL_Keycode up, down, left, right; };
static const struct stick_layout STICKS[2] = {
  {100, 188, 60, SDLK_w, SDLK_s, SDLK_a, SDLK_d},
  {490, 288, 60, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT},
};

static const SDL_Rect TEXTBOX = {25, 370, 385, 36};
static const SDL_Rect LIVEBTN = {424, 370, 130, 36};
static const SDL_Rect ENTERBTN = {568, 370, 127, 36};

// Script panel (right half).
static const SDL_Rect SCRIPT_REFRESH = {735, 20, 36, 36};
static const SDL_Rect SCRIPT_NAME = {781, 20, 336, 36};
static const SDL_Rect SCRIPT_RUN = {1129, 20, 126, 36};
static const SDL_Rect SCRIPT_LOG = {735, 76, 520, 330};

// Hover/focus targets for the animated selection ring.
enum { HOV_NONE = -1, HOV_STICK0 = 100, HOV_STICK1, HOV_TEXT, HOV_LIVE, HOV_ENTER,
       HOV_SCRIPT_NAME, HOV_SCRIPT_RUN, HOV_SCRIPT_REFRESH };

/* ---------------- theme (Switch 2 light) ---------------- */

static const SDL_Color C_BG = {235, 235, 235, 255};
static const SDL_Color C_BTN = {255, 255, 255, 255};
static const SDL_Color C_BTN_DOWN = {61, 124, 240, 255};
static const SDL_Color C_BTN_LATCH = {210, 232, 255, 255};
static const SDL_Color C_TEXT = {45, 45, 45, 255};
static const SDL_Color C_WHITE = {255, 255, 255, 255};
static const SDL_Color C_ACCENT = {52, 110, 230, 255};
static const SDL_Color C_RING_A = {24, 200, 255, 255};   // cyan
static const SDL_Color C_RING_B = {40, 90, 235, 255};    // blue
static const SDL_Color C_SHADOW = {0, 0, 0, 28};
static const SDL_Color C_WELL = {222, 222, 222, 255};

/* ---------------- state ---------------- */

struct textcache {
  char str[300];
  int size;
  SDL_Color color;
  SDL_Texture *tex;
  int w, h;
};

struct stick_state { float x, y; int dragging, latched; };

struct panel {
  SDL_Renderer *ren;
  TTF_Font *font_big, *font_small, *font_sym;
  struct textcache cache[160];
  int ncache;

  uint32_t mouse_bits, latch_bits, key_bits;
  int mouse_widget;
  struct stick_state stick[2];
  SDL_GameController *pad;

  float scale;
  int ox, oy;

  int hover;              // current hover target id
  uint32_t hover_since;   // when the pointer settled on it (tooltip delay)
  uint32_t last_anim_ms;

  int text_focus, live;
  char text[256];
  char submit[260];
  int have_submit;
  uint32_t flash_ms;      // Enter button flash
  int link_up;
  struct script *script;
  int script_sel;
  int refresh_down;       // refresh button held
  uint32_t refresh_ms;    // last refresh click (keeps a quick tap visibly blue)
};

/* ---------------- text ---------------- */

// Nintendo's UI face ships in app/fonts next to the binary; system fonts
// are the fallback. Symbols missing from it come from DejaVu (font_sym).
static TTF_Font *open_font(int px, int bold)
{
  char bundled[1024];
  if (sv_get_executable_dir(bundled, sizeof(bundled) - 40) == 0) {
    char font_path[1200];
    snprintf(font_path, sizeof(font_path), "%s/fonts/NintendoNTLG-DB.ttf", bundled);
    TTF_Font *f = TTF_OpenFont(font_path, px);
    if (!f) {
      snprintf(font_path, sizeof(font_path), "%s/../fonts/NintendoNTLG-DB.ttf", bundled);
      f = TTF_OpenFont(font_path, px);
    }
    if (f) { TTF_SetFontHinting(f, TTF_HINTING_LIGHT); return f; }
  }
#ifdef _WIN32
  const char *paths_b[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                           "C:\\Windows\\Fonts\\arial.ttf",
                           "C:\\Windows\\Fonts\\consola.ttf"};
  const char *paths_r[] = {"C:\\Windows\\Fonts\\segoeui.ttf",
                           "C:\\Windows\\Fonts\\arial.ttf",
                           "C:\\Windows\\Fonts\\consola.ttf"};
#else
  const char *paths_b[] = {"/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
  const char *paths_r[] = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                           "/usr/share/fonts/truetype/noto/NotoSans-Medium.ttf",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
#endif
  const char **paths = bold ? paths_b : paths_r;
  for (int i = 0; i < 3; i++) {
    TTF_Font *f = TTF_OpenFont(paths[i], px);
    if (f) { TTF_SetFontHinting(f, TTF_HINTING_LIGHT); return f; }
  }
  return NULL;
}

// Decode the first UTF-8 code point (enough to pick a font for a label).
static Uint32 first_cp(const char *s)
{
  const unsigned char *u = (const unsigned char *)s;
  if (u[0] < 0x80) return u[0];
  if ((u[0] & 0xe0) == 0xc0) return ((u[0] & 0x1f) << 6) | (u[1] & 0x3f);
  if ((u[0] & 0xf0) == 0xe0) return ((u[0] & 0x0f) << 12) | ((u[1] & 0x3f) << 6) | (u[2] & 0x3f);
  return 0xfffd;
}

static SDL_Texture *text_tex(struct panel *p, const char *s, int big, SDL_Color c, int *w, int *h)
{
  TTF_Font *f = big ? p->font_big : p->font_small;
  if (!f || !*s) return NULL;
  // Symbols (arrows, minus, home) live in DejaVu, not Noto.
  if (p->font_sym && !TTF_GlyphIsProvided32(f, first_cp(s))) f = p->font_sym;
  for (int i = 0; i < p->ncache; i++) {
    struct textcache *t = &p->cache[i];
    if (t->size == big && !strcmp(t->str, s) && t->color.r == c.r && t->color.g == c.g && t->color.b == c.b) {
      *w = t->w; *h = t->h;
      return t->tex;
    }
  }
  SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
  if (!surf) return NULL;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(p->ren, surf);
  *w = surf->w; *h = surf->h;
  SDL_FreeSurface(surf);
  if (p->ncache == (int)(sizeof p->cache / sizeof p->cache[0])) {
    SDL_DestroyTexture(p->cache[0].tex);
    memmove(&p->cache[0], &p->cache[1], sizeof p->cache[0] * (p->ncache - 1));
    p->ncache--;
  }
  struct textcache *t = &p->cache[p->ncache++];
  snprintf(t->str, sizeof t->str, "%s", s);
  t->size = big; t->color = c; t->tex = tex; t->w = *w; t->h = *h;
  return tex;
}

// align: 0 centre, 1 left, 2 right
static void draw_text(struct panel *p, const char *s, int big, SDL_Color c, int x, int cy, int align)
{
  int w, h;
  SDL_Texture *t = text_tex(p, s, big, c, &w, &h);
  if (!t) return;
  SDL_Rect dst = {align == 0 ? x - w / 2 : align == 1 ? x : x - w, cy - h / 2, w, h};
  SDL_RenderCopy(p->ren, t, NULL, &dst);
}

/* ---------------- rounded geometry ---------------- */

#define SEG 8  // segments per corner

static SDL_Color lerp_color(SDL_Color a, SDL_Color b, float t)
{
  SDL_Color c = {(Uint8)(a.r + (b.r - a.r) * t), (Uint8)(a.g + (b.g - a.g) * t),
                 (Uint8)(a.b + (b.b - a.b) * t), (Uint8)(a.a + (b.a - a.a) * t)};
  return c;
}

// Perimeter of a rounded rect grown by `grow` pixels, SEG*4 points clockwise.
static void perimeter(SDL_FRect rc, float rad, float grow, SDL_FPoint *out)
{
  float rr = rad + grow;
  if (rr < 0) rr = 0;
  float ccx[4] = {rc.x + rc.w - rad, rc.x + rc.w - rad, rc.x + rad, rc.x + rad};
  float ccy[4] = {rc.y + rad, rc.y + rc.h - rad, rc.y + rc.h - rad, rc.y + rad};
  int n = 0;
  for (int c = 0; c < 4; c++)
    for (int s = 0; s < SEG; s++) {
      float a = (float)(-M_PI / 2 + c * M_PI / 2 + s * (M_PI / 2) / (SEG - 1));
      out[n].x = ccx[c] + rr * cosf(a);
      out[n].y = ccy[c] + rr * sinf(a);
      n++;
    }
}

// Fill a rounded rect with an anti-aliased edge: a solid fan inside the
// perimeter plus a thin strip whose outer edge fades to alpha 0. If phase >= 0
// the colour cycles between the ring colours around the outline.
static SDL_BlendMode g_rrect_blend = SDL_BLENDMODE_BLEND;
static void arc_aa(SDL_Renderer *r, float cx, float cy, float rad, float thick,
                   float a0, float a1, SDL_Color col);
static void poly_aa(SDL_Renderer *r, const SDL_FPoint *pt, int n, SDL_Color col);

static void rrect(SDL_Renderer *r, SDL_FRect rc, float rad, SDL_Color col, float phase)
{
  if (rad > rc.w / 2) rad = rc.w / 2;
  if (rad > rc.h / 2) rad = rc.h / 2;
  enum { N = SEG * 4 };
  SDL_FPoint in[N], out[N];
  perimeter(rc, rad, -0.6f, in);
  perimeter(rc, rad, 0.8f, out);
  SDL_Vertex v[1 + 2 * N];
  int idx[N * 3 + N * 6];
  float cx = rc.x + rc.w / 2, cy = rc.y + rc.h / 2;
  v[0].position.x = cx; v[0].position.y = cy; v[0].color = col;
  v[0].tex_coord.x = v[0].tex_coord.y = 0;
  for (int i = 0; i < N; i++) {
    SDL_Color c = col;
    if (phase >= 0) {
      float ang = atan2f(in[i].y - cy, in[i].x - cx) / (2 * (float)M_PI);
      float t = 0.5f + 0.5f * sinf((ang + phase) * 2 * (float)M_PI * 2);
      c = lerp_color(C_RING_A, C_RING_B, t);
    }
    v[1 + i].position = in[i]; v[1 + i].color = c;
    v[1 + i].tex_coord.x = v[1 + i].tex_coord.y = 0;
    c.a = 0;
    v[1 + N + i].position = out[i]; v[1 + N + i].color = c;
    v[1 + N + i].tex_coord.x = v[1 + N + i].tex_coord.y = 0;
  }
  int m = 0;
  for (int i = 0; i < N; i++) {
    int a = 1 + i, b = 1 + (i + 1) % N, oa = 1 + N + i, ob = 1 + N + (i + 1) % N;
    idx[m++] = 0; idx[m++] = a; idx[m++] = b;            // interior fan
    idx[m++] = a; idx[m++] = oa; idx[m++] = ob;          // feather strip
    idx[m++] = a; idx[m++] = ob; idx[m++] = b;
  }
  SDL_SetRenderDrawBlendMode(r, g_rrect_blend);
  SDL_RenderGeometry(r, NULL, v, 1 + 2 * N, idx, m);
}

static void shadow(SDL_Renderer *r, SDL_FRect rc, float rad, float s)
{
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  for (int i = 3; i >= 1; i--) {
    SDL_FRect e = {rc.x - i * s, rc.y - i * s + 2 * s, rc.w + 2 * i * s, rc.h + 2 * i * s};
    SDL_Color c = C_SHADOW;
    c.a = (Uint8)(C_SHADOW.a / i);
    rrect(r, e, rad + i * s, c, -1);
  }
}

#define SX(p, v) ((p)->ox + (int)((v) * (p)->scale))
#define SY(p, v) ((p)->oy + (int)((v) * (p)->scale))
#define SS(p, v) ((int)((v) * (p)->scale))

static SDL_FRect widget_rect(struct panel *p, const struct widget *w)
{
  SDL_FRect rc = {SX(p, w->x - w->w / 2.f), SY(p, w->y - w->h / 2.f), w->w * p->scale, w->h * p->scale};
  return rc;
}

static SDL_FRect stick_rect(struct panel *p, int i)
{
  SDL_FRect rc = {SX(p, STICKS[i].x - STICKS[i].r), SY(p, STICKS[i].y - STICKS[i].r),
                  2 * STICKS[i].r * p->scale, 2 * STICKS[i].r * p->scale};
  return rc;
}

static SDL_FRect design_rect(struct panel *p, const SDL_Rect *r)
{
  SDL_FRect rc = {SX(p, r->x), SY(p, r->y), r->w * p->scale, r->h * p->scale};
  return rc;
}

/* ---------------- hit testing ---------------- */

static int widget_at(struct panel *p, int mx, int my)
{
  for (unsigned i = 0; i < NWIDGETS; i++) {
    const struct widget *w = &WIDGETS[i];
    if (!w->w) continue;  // keyboard/gamepad-only binding
    int cx = SX(p, w->x), cy = SY(p, w->y), hw = SS(p, w->w) / 2, hh = SS(p, w->h) / 2;
    if (w->shape == SH_CIRCLE) {
      int dx = mx - cx, dy = my - cy;
      if (dx * dx + dy * dy <= hw * hw) return (int)i;
    } else if (abs(mx - cx) <= hw && abs(my - cy) <= hh) {
      return (int)i;
    }
  }
  return -1;
}

static int stick_at(struct panel *p, int mx, int my)
{
  for (int i = 0; i < 2; i++) {
    int dx = mx - SX(p, STICKS[i].x), dy = my - SY(p, STICKS[i].y), r = SS(p, STICKS[i].r);
    if (dx * dx + dy * dy <= r * r) return i;
  }
  return -1;
}

static int in_rect(const SDL_Rect *r, struct panel *p, int mx, int my)
{
  return mx >= SX(p, r->x) && mx < SX(p, r->x + r->w) && my >= SY(p, r->y) && my < SY(p, r->y + r->h);
}

static int hover_at(struct panel *p, int mx, int my)
{
  if (in_rect(&TEXTBOX, p, mx, my)) return HOV_TEXT;
  if (in_rect(&LIVEBTN, p, mx, my)) return HOV_LIVE;
  if (in_rect(&ENTERBTN, p, mx, my)) return HOV_ENTER;
  if (in_rect(&SCRIPT_NAME, p, mx, my)) return HOV_SCRIPT_NAME;
  if (in_rect(&SCRIPT_RUN, p, mx, my)) return HOV_SCRIPT_RUN;
  if (in_rect(&SCRIPT_REFRESH, p, mx, my)) return HOV_SCRIPT_REFRESH;
  int w = widget_at(p, mx, my);
  if (w >= 0) return w;
  int s = stick_at(p, mx, my);
  if (s >= 0) return HOV_STICK0 + s;
  return HOV_NONE;
}

static SDL_FRect hover_rect(struct panel *p, int h, float *rad)
{
  SDL_FRect rc = {0, 0, 0, 0};
  *rad = 10 * p->scale;
  if (h >= 0 && h < (int)NWIDGETS) {
    rc = widget_rect(p, &WIDGETS[h]);
    if (WIDGETS[h].shape == SH_CIRCLE) *rad = rc.w / 2;
  } else if (h == HOV_STICK0 || h == HOV_STICK1) {
    rc = stick_rect(p, h - HOV_STICK0);
    *rad = rc.w / 2;
  } else if (h == HOV_TEXT) rc = design_rect(p, &TEXTBOX);
  else if (h == HOV_LIVE) rc = design_rect(p, &LIVEBTN);
  else if (h == HOV_ENTER) rc = design_rect(p, &ENTERBTN);
  else if (h == HOV_SCRIPT_NAME) rc = design_rect(p, &SCRIPT_NAME);
  else if (h == HOV_SCRIPT_RUN) rc = design_rect(p, &SCRIPT_RUN);
  return rc;
}

static void stick_from_mouse(struct panel *p, int i, int mx, int my)
{
  float dx = (float)(mx - SX(p, STICKS[i].x)) / SS(p, STICKS[i].r);
  float dy = (float)(my - SY(p, STICKS[i].y)) / SS(p, STICKS[i].r);
  float len = sqrtf(dx * dx + dy * dy);
  if (len > 1.0f) { dx /= len; dy /= len; }
  p->stick[i].x = dx;
  p->stick[i].y = -dy;
}

/* ---------------- public ---------------- */

struct panel *panel_create(SDL_Renderer *ren)
{
  struct panel *p = calloc(1, sizeof *p);
  p->ren = ren;
  p->mouse_widget = -1;
  p->hover = HOV_NONE;
  if (TTF_Init() == 0) {
    p->font_big = open_font(21, 1);
    p->font_small = open_font(12, 0);
#ifdef _WIN32
    p->font_sym = TTF_OpenFont("C:\\Windows\\Fonts\\segoeui.ttf", 19);
    if (!p->font_sym) p->font_sym = TTF_OpenFont("C:\\Windows\\Fonts\\arial.ttf", 19);
#else
    p->font_sym = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 19);
#endif
    if (p->font_sym) TTF_SetFontHinting(p->font_sym, TTF_HINTING_LIGHT);
    if (!p->font_big) fprintf(stderr, "panel: no TTF font found; labels disabled\n");
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      p->pad = SDL_GameControllerOpen(i);
      if (p->pad) { fprintf(stderr, "panel: gamepad %s\n", SDL_GameControllerName(p->pad)); break; }
    }
  }
  return p;
}

void panel_destroy(struct panel *p)
{
  if (!p) return;
  for (int i = 0; i < p->ncache; i++) SDL_DestroyTexture(p->cache[i].tex);
  if (p->pad) SDL_GameControllerClose(p->pad);
  if (p->font_big) TTF_CloseFont(p->font_big);
  if (p->font_small) TTF_CloseFont(p->font_small);
  if (p->font_sym) TTF_CloseFont(p->font_sym);
  TTF_Quit();
  free(p);
}

static void set_layout(struct panel *p, const SDL_Rect *area)
{
  float sx = (float)area->w / PANEL_DESIGN_W, sy = (float)area->h / PANEL_DESIGN_H;
  float scale = sx < sy ? sx : sy;
  p->scale = scale;
  p->ox = area->x + (area->w - (int)(PANEL_DESIGN_W * p->scale)) / 2;
  p->oy = area->y;
}

static void queue_submit(struct panel *p, const char *s)
{
  size_t n = strlen(p->submit);
  snprintf(p->submit + n, sizeof p->submit - n, "%s", s);
  p->have_submit = 1;
}

static void submit_line(struct panel *p, int newline)
{
  if (p->live) {
    // Live mode already sent every character; Enter just sends Enter.
    if (newline) queue_submit(p, "\n");
    p->text[0] = 0;
  } else {
    if (!p->text[0]) return;
    queue_submit(p, p->text);
    if (newline) queue_submit(p, "\n");
    p->text[0] = 0;
  }
  p->flash_ms = SDL_GetTicks();
}

static void set_focus(struct panel *p, int on)
{
  if (on == p->text_focus) return;
  p->text_focus = on;
  if (on) SDL_StartTextInput(); else SDL_StopTextInput();
  p->key_bits = 0;
}

static void type_chars(struct panel *p, const char *s, int allow_newline)
{
  size_t n = strlen(p->text);
  char live[2] = {0, 0};
  for (const char *c = s; *c; c++) {
    if ((unsigned char)*c >= 0x80) continue;  // firmware is ASCII-only
    if ((*c == '\n' || *c == '\r') && !allow_newline) continue;
    if (n + 1 < sizeof p->text) { p->text[n++] = *c; p->text[n] = 0; }
    if (p->live) { live[0] = *c; queue_submit(p, live); }
  }
}

int panel_event(struct panel *p, const SDL_Event *e, const SDL_Rect *area)
{
  set_layout(p, area);
  switch (e->type) {
  case SDL_MOUSEBUTTONDOWN: {
    int mx = e->button.x, my = e->button.y;
    if (my < area->y) { set_focus(p, 0); return 0; }
    if (in_rect(&TEXTBOX, p, mx, my)) { set_focus(p, 1); return 1; }
    if (in_rect(&ENTERBTN, p, mx, my)) { submit_line(p, 1); return 1; }
    if (in_rect(&LIVEBTN, p, mx, my)) { p->live = !p->live; p->text[0] = 0; return 1; }
    if (p->script && in_rect(&SCRIPT_NAME, p, mx, my)) {
      // Click cycles forward, right-click back; rescans so new files show up.
      if (!script_running(p->script)) {
        script_rescan(p->script);
        int n = script_count(p->script);
        if (n) p->script_sel = (p->script_sel + (e->button.button == SDL_BUTTON_RIGHT ? n - 1 : 1)) % n;
      }
      return 1;
    }
    if (p->script && in_rect(&SCRIPT_REFRESH, p, mx, my)) {
      p->refresh_down = 1;
      p->refresh_ms = SDL_GetTicks();
      if (!script_running(p->script)) {
        script_rescan(p->script);
        if (p->script_sel >= script_count(p->script)) p->script_sel = 0;
      }
      return 1;
    }
    if (p->script && in_rect(&SCRIPT_RUN, p, mx, my)) {
      if (script_running(p->script)) script_stop(p->script);
      else { script_rescan(p->script); script_start(p->script, p->script_sel); }
      return 1;
    }
    set_focus(p, 0);
    int wi = widget_at(p, mx, my);
    if (wi >= 0) {
      if (e->button.button == SDL_BUTTON_RIGHT) p->latch_bits ^= WIDGETS[wi].bit;
      else { p->mouse_bits |= WIDGETS[wi].bit; p->mouse_widget = wi; }
      return 1;
    }
    int si = stick_at(p, mx, my);
    if (si >= 0) {
      if (e->button.button == SDL_BUTTON_RIGHT) {
        p->stick[si].latched = !p->stick[si].latched;
        if (!p->stick[si].latched) p->stick[si].x = p->stick[si].y = 0;
      } else {
        p->stick[si].dragging = 1;
        p->stick[si].latched = 0;
        stick_from_mouse(p, si, mx, my);
      }
      return 1;
    }
    return 1;
  }
  case SDL_MOUSEMOTION: {
    int h = e->motion.y >= area->y ? hover_at(p, e->motion.x, e->motion.y) : HOV_NONE;
    if (h != p->hover) { p->hover = h; p->hover_since = SDL_GetTicks(); }
    for (int i = 0; i < 2; i++)
      if (p->stick[i].dragging) stick_from_mouse(p, i, e->motion.x, e->motion.y);
    return 1;
  }
  case SDL_MOUSEBUTTONUP:
    if (e->button.button == SDL_BUTTON_LEFT) {
      p->refresh_down = 0;
      if (p->mouse_widget >= 0) { p->mouse_bits &= ~WIDGETS[p->mouse_widget].bit; p->mouse_widget = -1; }
      for (int i = 0; i < 2; i++)
        if (p->stick[i].dragging) { p->stick[i].dragging = 0; p->stick[i].x = p->stick[i].y = 0; }
    }
    return 0;
  case SDL_TEXTINPUT:
    if (p->text_focus) { type_chars(p, e->text.text, 0); return 1; }
    return 0;
  case SDL_KEYDOWN:
  case SDL_KEYUP: {
    int down = e->type == SDL_KEYDOWN;
    SDL_Keycode k = e->key.keysym.sym;
    if (p->text_focus) {
      if (!down) return 1;
      if (k == SDLK_ESCAPE) set_focus(p, 0);
      else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) submit_line(p, !(e->key.keysym.mod & KMOD_SHIFT));
      else if (k == SDLK_TAB) type_chars(p, "\t", 1);
      else if (k == SDLK_CAPSLOCK) { static const char caps[2] = {SV_KEY_CAPSLOCK, 0}; queue_submit(p, caps); }
      else if (k == SDLK_BACKSPACE) {
        size_t n = strlen(p->text);
        if (n) p->text[n - 1] = 0;
        if (p->live) queue_submit(p, "\b");
      } else if (k == SDLK_v && (e->key.keysym.mod & KMOD_CTRL)) {
        char *clip = SDL_GetClipboardText();
        if (clip) { type_chars(p, clip, 0); SDL_free(clip); }
      }
      return 1;
    }
    if (e->key.repeat) return 1;
    if (k == SDLK_RETURN && !down) { set_focus(p, 1); return 1; }
    for (unsigned i = 0; i < NWIDGETS; i++)
      if (WIDGETS[i].keycode == k) {
        if (down) p->key_bits |= WIDGETS[i].bit; else p->key_bits &= ~WIDGETS[i].bit;
        return 1;
      }
    return 0;
  }
  case SDL_CONTROLLERDEVICEADDED:
    if (!p->pad) {
      p->pad = SDL_GameControllerOpen(e->cdevice.which);
      if (p->pad) fprintf(stderr, "panel: gamepad %s\n", SDL_GameControllerName(p->pad));
    }
    return 1;
  case SDL_CONTROLLERDEVICEREMOVED:
    if (p->pad && e->cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p->pad))) {
      SDL_GameControllerClose(p->pad);
      p->pad = NULL;
    }
    return 1;
  }
  return 0;
}

static int16_t clamp16(float v)
{
  if (v > 1.0f) v = 1.0f;
  if (v < -1.0f) v = -1.0f;
  return (int16_t)(v * 32767);
}

void panel_get_state(struct panel *p, struct link_state *out)
{
  uint32_t bits = p->mouse_bits | p->latch_bits | p->key_bits;
  float sx[2], sy[2];
  for (int i = 0; i < 2; i++) { sx[i] = p->stick[i].x; sy[i] = p->stick[i].y; }

  if (!p->text_focus) {
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < 2; i++) {
      const struct stick_layout *sl = &STICKS[i];
      float kx = (ks[SDL_GetScancodeFromKey(sl->right)] ? 1.f : 0.f) - (ks[SDL_GetScancodeFromKey(sl->left)] ? 1.f : 0.f);
      float ky = (ks[SDL_GetScancodeFromKey(sl->up)] ? 1.f : 0.f) - (ks[SDL_GetScancodeFromKey(sl->down)] ? 1.f : 0.f);
      if (kx || ky) {
        float len = sqrtf(kx * kx + ky * ky);
        sx[i] = kx / len; sy[i] = ky / len;
      }
    }
  }
  if (p->pad) {
    for (unsigned i = 0; i < NWIDGETS; i++)
      if (WIDGETS[i].pad != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(p->pad, WIDGETS[i].pad))
        bits |= WIDGETS[i].bit;
    if (SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) bits |= SV_BTN_ZL;
    if (SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) bits |= SV_BTN_ZR;
    float ax[2] = {SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.f,
                   SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.f};
    float ay[2] = {-SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.f,
                   -SDL_GameControllerGetAxis(p->pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.f};
    for (int i = 0; i < 2; i++)
      if (fabsf(ax[i]) > 0.08f || fabsf(ay[i]) > 0.08f) { sx[i] = ax[i]; sy[i] = ay[i]; }
  }

  out->buttons = bits & BTN_MASK;
  out->lx = clamp16(sx[0]); out->ly = clamp16(sy[0]);
  out->rx = clamp16(sx[1]); out->ry = clamp16(sy[1]);
  int up = !!(bits & DP_UP), down = !!(bits & DP_DOWN), left = !!(bits & DP_LEFT), right = !!(bits & DP_RIGHT);
  if (up && down) up = down = 0;
  if (left && right) left = right = 0;
  static const uint8_t hat[3][3] = {
    {SV_HAT_UPLEFT, SV_HAT_UP, SV_HAT_UPRIGHT},
    {SV_HAT_LEFT, SV_HAT_CENTER, SV_HAT_RIGHT},
    {SV_HAT_DOWNLEFT, SV_HAT_DOWN, SV_HAT_DOWNRIGHT},
  };
  out->hat = hat[1 - up + down][1 - left + right];
}

int panel_take_text(struct panel *p, char *buf, size_t len)
{
  if (!p->have_submit) return 0;
  snprintf(buf, len, "%s", p->submit);
  p->submit[0] = 0;
  p->have_submit = 0;
  return 1;
}

int panel_text_focused(struct panel *p) { return p->text_focus; }
void panel_set_script(struct panel *p, struct script *s) { p->script = s; }
void panel_set_link(struct panel *p, int up) { p->link_up = up; }

/* ---------------- render ---------------- */

// Convex polygon with a feathered (anti-aliased) edge.
static void poly_aa(SDL_Renderer *r, const SDL_FPoint *pt, int n, SDL_Color col)
{
  SDL_Vertex v[64];
  int idx[192], m = 0;
  float cx = 0, cy = 0;
  for (int i = 0; i < n; i++) { cx += pt[i].x; cy += pt[i].y; }
  cx /= n; cy /= n;
  v[0].position.x = cx; v[0].position.y = cy; v[0].color = col;
  v[0].tex_coord.x = v[0].tex_coord.y = 0;
  for (int i = 0; i < n; i++) {
    // Outward direction from the centroid approximates the vertex normal.
    float dx = pt[i].x - cx, dy = pt[i].y - cy, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f) len = 1;
    dx /= len; dy /= len;
    v[1 + i].position.x = pt[i].x - dx * 0.6f; v[1 + i].position.y = pt[i].y - dy * 0.6f;
    v[1 + i].color = col; v[1 + i].tex_coord.x = v[1 + i].tex_coord.y = 0;
    SDL_Color t = col; t.a = 0;
    v[1 + n + i].position.x = pt[i].x + dx * 0.9f; v[1 + n + i].position.y = pt[i].y + dy * 0.9f;
    v[1 + n + i].color = t; v[1 + n + i].tex_coord.x = v[1 + n + i].tex_coord.y = 0;
  }
  for (int i = 0; i < n; i++) {
    int a = 1 + i, b = 1 + (i + 1) % n, oa = 1 + n + i, ob = 1 + n + (i + 1) % n;
    idx[m++] = 0; idx[m++] = a; idx[m++] = b;
    idx[m++] = a; idx[m++] = oa; idx[m++] = ob;
    idx[m++] = a; idx[m++] = ob; idx[m++] = b;
  }
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_RenderGeometry(r, NULL, v, 1 + 2 * n, idx, m);
}

// The face of a button: a text label, or a drawn icon for Capture / Home.
static void draw_face(struct panel *p, const struct widget *w, SDL_FRect rc, SDL_Color fill, SDL_Color fg)
{
  float cx = rc.x + rc.w / 2, cy = rc.y + rc.h / 2, u = rc.w;
  if (w->bit == SV_BTN_CAPTURE) {
    // Rounded square outline with a dot: the Switch capture glyph.
    float sz = u * 0.46f, t = u * 0.07f;
    SDL_FRect o = {cx - sz / 2, cy - sz / 2, sz, sz};
    SDL_FRect i = {o.x + t, o.y + t, sz - 2 * t, sz - 2 * t};
    rrect(p->ren, o, u * 0.1f, fg, -1);
    rrect(p->ren, i, u * 0.06f, fill, -1);
    float d = u * 0.16f;
    SDL_FRect dot = {cx - d / 2, cy - d / 2, d, d};
    rrect(p->ren, dot, d / 2, fg, -1);
  } else if (w->bit == SV_BTN_HOME) {
    // House: roof triangle + body with a door cut out.
    float hw = u * 0.26f, roof_y = cy - u * 0.27f, eave_y = cy - u * 0.02f;
    SDL_FPoint roof[3] = {{cx, roof_y}, {cx + hw * 1.15f, eave_y}, {cx - hw * 1.15f, eave_y}};
    poly_aa(p->ren, roof, 3, fg);
    SDL_FRect body = {cx - hw * 0.8f, eave_y - u * 0.02f, hw * 1.6f, u * 0.26f};
    rrect(p->ren, body, u * 0.03f, fg, -1);
    SDL_FRect door = {cx - hw * 0.22f, eave_y + u * 0.08f, hw * 0.44f, u * 0.17f};
    rrect(p->ren, door, u * 0.03f, fill, -1);
  } else {
    draw_text(p, w->label, 1, fg, (int)cx, (int)cy, 0);
  }
}

static void button(struct panel *p, const struct widget *w, SDL_FRect rc, float rad, SDL_Color fill, SDL_Color fg)
{
  shadow(p->ren, rc, rad, 1.2f * p->scale);
  rrect(p->ren, rc, rad, fill, -1);
  draw_face(p, w, rc, fill, fg);
}

// Keyboard shortcut for the hovered control, faded in after a 1 s dwell.
static void draw_tooltip(struct panel *p, const SDL_Rect *area)
{
  if (p->hover == HOV_NONE || p->text_focus || p->mouse_widget >= 0) return;
  const char *tip = NULL;
  if (p->hover >= 0 && p->hover < (int)NWIDGETS) tip = WIDGETS[p->hover].key;
  else if (p->hover == HOV_STICK0) tip = "WASD";
  else if (p->hover == HOV_STICK1) tip = "Arrows";
  if (!tip) return;
  uint32_t dwell = SDL_GetTicks() - p->hover_since;
  if (dwell < 1000) return;
  float a = (dwell - 1000) / 150.f;
  if (a > 1) a = 1;

  float rad;
  SDL_FRect t = hover_rect(p, p->hover, &rad);
  int tw = 0, th = 0;
  SDL_Color fg = {255, 255, 255, 255};
  SDL_Texture *tex = text_tex(p, tip, 0, fg, &tw, &th);
  if (!tex) return;
  float padx = 10 * p->scale, pady = 5 * p->scale;
  SDL_FRect box = {t.x + t.w / 2 - tw / 2.f - padx, t.y - th - 2 * pady - 8 * p->scale,
                   tw + 2 * padx, th + 2 * pady};
  if (box.x < area->x + 4) box.x = area->x + 4;
  if (box.x + box.w > area->x + area->w - 4) box.x = area->x + area->w - 4 - box.w;
  SDL_Color bg = {30, 30, 30, (Uint8)(220 * a)};
  rrect(p->ren, box, box.h / 2, bg, -1);
  SDL_SetTextureAlphaMod(tex, (Uint8)(255 * a));
  SDL_Rect dst = {(int)(box.x + padx), (int)(box.y + pady), tw, th};
  SDL_RenderCopy(p->ren, tex, NULL, &dst);
  SDL_SetTextureAlphaMod(tex, 255);
}

static void render_script_panel(struct panel *p)
{
  SDL_Renderer *r = p->ren;
  if (!p->script) return;
  int running = script_running(p->script);
  int n = script_count(p->script);
  if (p->script_sel >= n) p->script_sel = 0;

  // Refresh: rescan scripts/ (circular arrow)
  {
    SDL_FRect fb = design_rect(p, &SCRIPT_REFRESH);
    int lit = p->refresh_down || SDL_GetTicks() - p->refresh_ms < 150;
    SDL_Color ic = lit ? C_WHITE : C_TEXT;
    shadow(r, fb, fb.w / 2, 1.0f * p->scale);
    rrect(r, fb, fb.w / 2, lit ? C_BTN_DOWN : C_BTN, -1);
    float cx = fb.x + fb.w / 2, cy = fb.y + fb.h / 2, u = fb.w / 36.f;
    float rad = 8.5f * u, a0 = (float)(-M_PI * 0.35), a1 = (float)(M_PI * 1.25);
    arc_aa(r, cx, cy, rad, 2.4f * u, a0, a1, ic);
    // Arrowhead at the open end (a0), pointing along the arc's direction.
    float ex = cx + rad * cosf(a0), ey = cy + rad * sinf(a0);
    float tx = -sinf(a0), ty = cosf(a0);  // tangent toward increasing angle
    float nx = cosf(a0), ny = sinf(a0);   // outward normal
    float h = 4.6f * u;
    SDL_FPoint tri[3] = {{ex - tx * h, ey - ty * h},
                         {ex + nx * h * 0.9f + tx * h * 0.35f, ey + ny * h * 0.9f + ty * h * 0.35f},
                         {ex - nx * h * 0.9f + tx * h * 0.35f, ey - ny * h * 0.9f + ty * h * 0.35f}};
    poly_aa(r, tri, 3, ic);
  }

  // Script picker pill
  SDL_FRect nb = design_rect(p, &SCRIPT_NAME);
  shadow(r, nb, nb.h / 2, 1.0f * p->scale);
  rrect(r, nb, nb.h / 2, C_BTN, -1);
  char label[160];
  snprintf(label, sizeof label, "%s", n ? script_name(p->script, p->script_sel) : "No scripts found");
  size_t ln = strlen(label);
  if (ln > 4 && !strcmp(label + ln - 4, ".lua")) label[ln - 4] = 0;
  draw_text(p, label, 0, n ? C_TEXT : (SDL_Color){150, 150, 150, 255},
            (int)(nb.x + SS(p, 18)), (int)(nb.y + nb.h / 2), 1);
  if (n > 1) {
    char cnt[32];
    snprintf(cnt, sizeof cnt, "%d / %d", p->script_sel + 1, n);
    draw_text(p, cnt, 0, (SDL_Color){150, 150, 150, 255}, (int)(nb.x + nb.w - SS(p, 18)), (int)(nb.y + nb.h / 2), 2);
  }

  // Run / Stop
  SDL_FRect rb = design_rect(p, &SCRIPT_RUN);
  shadow(r, rb, rb.h / 2, 1.0f * p->scale);
  rrect(r, rb, rb.h / 2, running ? (SDL_Color){215, 70, 70, 255} : C_ACCENT, -1);
  draw_text(p, running ? "Stop" : "Run", 1, C_WHITE, (int)(rb.x + rb.w / 2), (int)(rb.y + rb.h / 2), 0);

  // Log
  SDL_FRect lb = design_rect(p, &SCRIPT_LOG);
  shadow(r, lb, 9 * p->scale, 1.0f * p->scale);
  rrect(r, lb, 9 * p->scale, C_BTN, -1);
  static char lines[SCRIPT_LOG_LINES][SCRIPT_LOG_COLS];
  int lh = TTF_FontLineSkip(p->font_small ? p->font_small : p->font_big);
  int fit = (int)((lb.h - SS(p, 20)) / (lh ? lh : 14));
  if (fit > SCRIPT_LOG_LINES) fit = SCRIPT_LOG_LINES;
  int nl = script_log(p->script, lines, fit);
  // Keep long lines inside the card instead of spilling over the panel.
  SDL_Rect clip = {(int)lb.x + 2, (int)lb.y + 2, (int)lb.w - 4, (int)lb.h - 4};
  SDL_RenderSetClipRect(r, &clip);
  for (int i = 0; i < nl; i++) {
    const char *t = lines[i];
    SDL_Color c = !strncmp(t, "error", 5) ? (SDL_Color){200, 60, 60, 255}
                : t[0] == '>' ? C_ACCENT : C_TEXT;
    draw_text(p, t, 0, c, (int)(lb.x + SS(p, 14)), (int)(lb.y + SS(p, 10) + lh * i + lh / 2), 1);
  }
  SDL_RenderSetClipRect(r, NULL);
}

void panel_render(struct panel *p, const SDL_Rect *area, const char *status)
{
  set_layout(p, area);
  SDL_Renderer *r = p->ren;
  uint32_t now = SDL_GetTicks();
  uint32_t active = p->mouse_bits | p->latch_bits | p->key_bits;
  struct link_state st;
  panel_get_state(p, &st);

  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(r, C_BG.r, C_BG.g, C_BG.b, 255);
  SDL_RenderFillRect(r, area);
  // Top edge: subtle divider against the video.
  SDL_SetRenderDrawColor(r, 200, 200, 200, 255);
  SDL_RenderDrawLine(r, area->x, area->y, area->x + area->w, area->y);

  // Buttons
  for (unsigned i = 0; i < NWIDGETS; i++) {
    const struct widget *w = &WIDGETS[i];
    if (!w->w) continue;
    int on = (active & w->bit) || (w->bit <= BTN_MASK && (st.buttons & w->bit));
    int latched = p->latch_bits & w->bit;
    SDL_FRect rc = widget_rect(p, w);
    float rad = w->shape == SH_CIRCLE ? rc.w / 2 : 9 * p->scale;
    SDL_Color fill = on ? C_BTN_DOWN : latched ? C_BTN_LATCH : C_BTN;
    button(p, w, rc, rad, fill, on ? C_WHITE : C_TEXT);
  }

  // Sticks
  for (int i = 0; i < 2; i++) {
    SDL_FRect rc = stick_rect(p, i);
    int cx = SX(p, STICKS[i].x), cy = SY(p, STICKS[i].y), rad = SS(p, STICKS[i].r);
    rrect(r, rc, rc.w / 2, C_WELL, -1);
    float x = i == 0 ? st.lx / 32767.f : st.rx / 32767.f, y = i == 0 ? st.ly / 32767.f : st.ry / 32767.f;
    int kx = cx + (int)(x * rad * 0.62f), ky = cy - (int)(y * rad * 0.62f);
    int kr = (int)(rad * 0.42f);
    SDL_FRect knob = {kx - kr, ky - kr, 2.f * kr, 2.f * kr};
    SDL_Color kc = p->stick[i].latched ? C_BTN_LATCH : (x || y) ? C_BTN_DOWN : C_BTN;
    shadow(r, knob, kr, 1.2f * p->scale);
    rrect(r, knob, kr, kc, -1);
  }

  // Text entry
  SDL_FRect tb = design_rect(p, &TEXTBOX);
  shadow(r, tb, 9 * p->scale, 1.0f * p->scale);
  rrect(r, tb, 9 * p->scale, C_BTN, -1);
  int tx = (int)(tb.x + SS(p, 14)), ty = (int)(tb.y + tb.h / 2);
  // Show the tail of the text that fits inside the box (it scrolls left as
  // you type), so long input never spills past the edge.
  const char *vis = p->text;
  int tw = 0;
  int maxw = (int)(tb.w - SS(p, 28));
  TTF_Font *tf = p->font_small ? p->font_small : p->font_big;
  if (tf && p->text[0]) {
    TTF_SizeUTF8(tf, vis, &tw, NULL);
    while (*vis && tw > maxw) { vis++; TTF_SizeUTF8(tf, vis, &tw, NULL); }
    draw_text(p, vis, 0, C_TEXT, tx, ty, 1);
  }
  if (p->text_focus && (now / 500) % 2) {
    SDL_SetRenderDrawColor(r, C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 255);
    SDL_Rect cur = {tx + (*vis ? tw + 1 : 0), (int)(tb.y + SS(p, 9)), 2, (int)(tb.h - SS(p, 18))};
    SDL_RenderFillRect(r, &cur);
  }

  // Live toggle pill
  SDL_FRect lb = design_rect(p, &LIVEBTN);
  shadow(r, lb, lb.h / 2, 1.0f * p->scale);
  rrect(r, lb, lb.h / 2, p->live ? C_BTN_DOWN : C_BTN, -1);
  draw_text(p, p->live ? "Live Typing" : "Line Mode", 0, p->live ? C_WHITE : C_TEXT,
            (int)(lb.x + lb.w / 2), (int)(lb.y + lb.h / 2), 0);

  // Enter button (Switch "OK" blue)
  SDL_FRect eb = design_rect(p, &ENTERBTN);
  int flash = now - p->flash_ms < 150;
  shadow(r, eb, 9 * p->scale, 1.0f * p->scale);
  rrect(r, eb, 9 * p->scale, flash ? C_RING_A : C_ACCENT, -1);
  draw_text(p, "Enter", 1, C_WHITE, (int)(eb.x + eb.w / 2), (int)(eb.y + eb.h / 2), 0);

  (void)status;
  draw_tooltip(p, area);
  render_script_panel(p);
  // Pico link dot, left of the text box: green when it answers pings.
  {
    float d = 9 * p->scale;
    SDL_FRect dot = {SX(p, 10) - d / 2, SY(p, 388) - d / 2, d, d};
    rrect(r, dot, d / 2, p->link_up ? (SDL_Color){70, 190, 90, 255} : (SDL_Color){215, 80, 80, 255}, -1);
  }
}

/* ---------------- mute overlay ---------------- */

// Thick arc with feathered edges (sound waves).
static void arc_aa(SDL_Renderer *r, float cx, float cy, float rad, float thick,
                   float a0, float a1, SDL_Color col)
{
  enum { N = 24 };
  SDL_Vertex v[(N + 1) * 4];
  int idx[N * 18], m = 0;
  float rs[4] = {rad - thick / 2 - 0.8f, rad - thick / 2 + 0.5f, rad + thick / 2 - 0.5f, rad + thick / 2 + 0.8f};
  for (int i = 0; i <= N; i++) {
    float a = a0 + (a1 - a0) * i / N;
    for (int k = 0; k < 4; k++) {
      SDL_Vertex *vv = &v[i * 4 + k];
      vv->position.x = cx + rs[k] * cosf(a);
      vv->position.y = cy + rs[k] * sinf(a);
      vv->color = col;
      if (k == 0 || k == 3) vv->color.a = 0;
      vv->tex_coord.x = vv->tex_coord.y = 0;
    }
  }
  for (int i = 0; i < N; i++)
    for (int k = 0; k < 3; k++) {
      int a = i * 4 + k, b = a + 1, c = a + 4, d = a + 5;
      idx[m++] = a; idx[m++] = c; idx[m++] = b;
      idx[m++] = b; idx[m++] = c; idx[m++] = d;
    }
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  SDL_RenderGeometry(r, NULL, v, (N + 1) * 4, idx, m);
}

// Line segment with round-ish feathered ends, as a convex quad.
static void line_aa(SDL_Renderer *r, float x0, float y0, float x1, float y1, float thick, SDL_Color col)
{
  float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 1e-3f) return;
  float nx = -dy / len * thick / 2, ny = dx / len * thick / 2;
  SDL_FPoint q[4] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
  poly_aa(r, q, 4, col);
}

void panel_draw_mute(struct panel *p, SDL_FRect rc, int muted, int hover, float alpha)
{
  SDL_Renderer *r = p->ren;
  float d = rc.w, cx = rc.x + d / 2, cy = rc.y + d / 2, u = d / 40.f;
  if (alpha <= 0.01f) return;
  SDL_Color bg = {0, 0, 0, (Uint8)((hover ? 175 : 125) * alpha)};
  SDL_Color fg = {255, 255, 255, (Uint8)((hover ? 255 : 235) * alpha)};
  rrect(r, rc, d / 2, bg, -1);

  // Speaker: body + cone, shifted left to balance the waves / X.
  float sx = cx - 6.5f * u;
  SDL_FRect body = {sx - 5.5f * u, cy - 4 * u, 5 * u, 8 * u};
  rrect(r, body, 1.2f * u, fg, -1);
  SDL_FPoint cone[4] = {{sx - 1.0f * u, cy - 4 * u}, {sx + 5.5f * u, cy - 9.5f * u},
                        {sx + 5.5f * u, cy + 9.5f * u}, {sx - 1.0f * u, cy + 4 * u}};
  poly_aa(r, cone, 4, fg);

  float wx = sx + 7.5f * u;
  if (muted) {
    float s = 4.2f * u, xc = wx + 5 * u;
    line_aa(r, xc - s, cy - s, xc + s, cy + s, 2.4f * u, fg);
    line_aa(r, xc - s, cy + s, xc + s, cy - s, 2.4f * u, fg);
  } else {
    float span = (float)M_PI / 3.4f;
    arc_aa(r, wx - 1.5f * u, cy, 5.0f * u, 2.2f * u, -span, span, fg);
    arc_aa(r, wx - 1.5f * u, cy, 10.0f * u, 2.2f * u, -span, span, fg);
  }
}

