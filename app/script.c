#include "script.h"
#include "switchview_proto.h"

#include <SDL2/SDL.h>

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

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef _WIN32
#include <unistd.h>
#include <strings.h>
#endif

#define MAX_SCRIPTS 64
#define MAX_RULES 64
#define MAX_RULE_WORDS 8
#define DEFAULT_HOLD 0.12  // shorter taps can fall between controller reports

struct rule {
  char label[48];
  char words[MAX_RULE_WORDS][64];
  int nwords;
};

struct script {
  struct ocr *ocr;
  char dir[1024];
  char names[MAX_SCRIPTS][260];
  int count;

  SDL_Thread *th;
  atomic_int running, stop;

  SDL_mutex *lock;  // guards everything below
  struct link_state input;
  char textq[1024];
  size_t textq_len;
  char screen[48];
  char log[SCRIPT_LOG_LINES][SCRIPT_LOG_COLS];
  int log_head, log_count;
  struct rule rules[MAX_RULES];
  int nrules;
  char path[1300];
  char hl[MAX_RULE_WORDS][64];
  int nhl;
  atomic_int leds;
};

static const struct { const char *name; uint32_t bit; } BUTTONS[] = {
  {"A", SV_BTN_A}, {"B", SV_BTN_B}, {"X", SV_BTN_X}, {"Y", SV_BTN_Y},
  {"L", SV_BTN_L}, {"R", SV_BTN_R}, {"ZL", SV_BTN_ZL}, {"ZR", SV_BTN_ZR},
  {"PLUS", SV_BTN_PLUS}, {"+", SV_BTN_PLUS}, {"MINUS", SV_BTN_MINUS}, {"-", SV_BTN_MINUS},
  {"HOME", SV_BTN_HOME}, {"CAPTURE", SV_BTN_CAPTURE}, {"L3", SV_BTN_L3}, {"R3", SV_BTN_R3},
};

static const struct { const char *name; uint8_t hat; } HATS[] = {
  {"up", SV_HAT_UP}, {"upright", SV_HAT_UPRIGHT}, {"right", SV_HAT_RIGHT},
  {"downright", SV_HAT_DOWNRIGHT}, {"down", SV_HAT_DOWN}, {"downleft", SV_HAT_DOWNLEFT},
  {"left", SV_HAT_LEFT}, {"upleft", SV_HAT_UPLEFT}, {"center", SV_HAT_CENTER},
};

/* ---------------- shared state helpers ---------------- */

static void logf_(struct script *s, const char *fmt, ...)
{
  char line[SCRIPT_LOG_COLS];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fprintf(stderr, "script: %s\n", line);
  SDL_LockMutex(s->lock);
  int idx = (s->log_head + s->log_count) % SCRIPT_LOG_LINES;
  if (s->log_count == SCRIPT_LOG_LINES) s->log_head = (s->log_head + 1) % SCRIPT_LOG_LINES;
  else s->log_count++;
  snprintf(s->log[idx], SCRIPT_LOG_COLS, "%s", line);
  SDL_UnlockMutex(s->lock);
}

static struct script *self(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "sv_script");
  struct script *s = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return s;
}

// Sleep in small slices so Stop takes effect immediately.
static void nap(lua_State *L, struct script *s, double seconds)
{
  uint32_t end = SDL_GetTicks() + (uint32_t)(seconds * 1000);
  while ((int32_t)(end - SDL_GetTicks()) > 0) {
    if (atomic_load(&s->stop)) luaL_error(L, "stopped");
    sv_usleep(5000);
  }
  if (atomic_load(&s->stop)) luaL_error(L, "stopped");
}

static uint32_t button_bits(lua_State *L, int first)
{
  uint32_t bits = 0;
  for (int i = first; i <= lua_gettop(L); i++) {
    if (lua_type(L, i) != LUA_TSTRING) continue;
    const char *n = lua_tostring(L, i);
    unsigned k;
    for (k = 0; k < sizeof BUTTONS / sizeof BUTTONS[0]; k++)
      if (!strcasecmp(n, BUTTONS[k].name)) { bits |= BUTTONS[k].bit; break; }
    if (k == sizeof BUTTONS / sizeof BUTTONS[0]) luaL_error(L, "unknown button '%s'", n);
  }
  return bits;
}

// Collapse spaces: OCR drops and adds them unpredictably.
static void squash(const char *in, char *out, size_t len)
{
  size_t n = 0;
  for (; *in && n + 1 < len; in++) if (*in != ' ') out[n++] = *in;
  out[n] = 0;
}

static const char *classify(struct script *s, const char *text)
{
  static char squashed[8192];  // script thread only
  squash(text, squashed, sizeof squashed);
  SDL_LockMutex(s->lock);
  const char *label = "unknown";
  for (int i = 0; i < s->nrules; i++) {
    int all = 1;
    for (int w = 0; w < s->rules[i].nwords && all; w++)
      if (!strstr(squashed, s->rules[i].words[w])) all = 0;
    if (all) {
      label = s->rules[i].label;
      s->nhl = s->rules[i].nwords;
      memcpy(s->hl, s->rules[i].words, sizeof s->hl);
      break;
    }
  }
  snprintf(s->screen, sizeof s->screen, "%s", label);
  SDL_UnlockMutex(s->lock);
  return label;
}

// Block until an OCR result for a frame requested at/after `since` exists.
static void fresh_text(lua_State *L, struct script *s, uint32_t since, char *buf, size_t len)
{
  for (;;) {
    uint32_t started;
    if (ocr_latest(s->ocr, buf, len, &started) && (int32_t)(started - since) >= 0) return;
    nap(L, s, 0.02);
  }
}

/* ---------------- Lua API ---------------- */

static int l_log(lua_State *L)
{
  int n = lua_gettop(L), parts = 0;
  for (int i = 1; i <= n; i++) {
    if (i > 1) { lua_pushliteral(L, " "); parts++; }
    luaL_tolstring(L, i, NULL);
    parts++;
  }
  if (parts) lua_concat(L, parts); else lua_pushliteral(L, "");
  logf_(self(L), "%s", lua_tostring(L, -1));
  return 0;
}

static int l_sleep(lua_State *L)
{
  nap(L, self(L), luaL_checknumber(L, 1));
  return 0;
}

static int l_press(lua_State *L)
{
  struct script *s = self(L);
  uint32_t bits = button_bits(L, 1);
  SDL_LockMutex(s->lock);
  s->input.buttons |= bits;
  SDL_UnlockMutex(s->lock);
  return 0;
}

static int l_release(lua_State *L)
{
  struct script *s = self(L);
  uint32_t bits = lua_gettop(L) ? button_bits(L, 1) : 0xffffffffu;
  SDL_LockMutex(s->lock);
  s->input.buttons &= ~bits;
  SDL_UnlockMutex(s->lock);
  return 0;
}

// tap("A") / tap("A", 0.3) / tap("L", "R", 0.2)
static int l_tap(lua_State *L)
{
  struct script *s = self(L);
  int top = lua_gettop(L);
  double hold = DEFAULT_HOLD;
  if (top >= 2 && lua_type(L, top) == LUA_TNUMBER) {
    hold = lua_tonumber(L, top);
    top--;
  }
  uint32_t bits = 0;
  for (int i = 1; i <= top; i++) {
    const char *n = luaL_checkstring(L, i);
    unsigned k;
    for (k = 0; k < sizeof BUTTONS / sizeof BUTTONS[0]; k++)
      if (!strcasecmp(n, BUTTONS[k].name)) { bits |= BUTTONS[k].bit; break; }
    if (k == sizeof BUTTONS / sizeof BUTTONS[0]) return luaL_error(L, "unknown button '%s'", n);
  }
  SDL_LockMutex(s->lock);
  s->input.buttons |= bits;
  SDL_UnlockMutex(s->lock);
  nap(L, s, hold);
  SDL_LockMutex(s->lock);
  s->input.buttons &= ~bits;
  SDL_UnlockMutex(s->lock);
  nap(L, s, 0.05);  // gap before next action
  return 0;
}

// dpad("up") / dpad("down", 0.2) / dpad() releases
static int l_dpad(lua_State *L)
{
  struct script *s = self(L);
  if (lua_gettop(L) == 0) {
    SDL_LockMutex(s->lock);
    s->input.hat = SV_HAT_CENTER;
    SDL_UnlockMutex(s->lock);
    return 0;
  }
  const char *d = luaL_checkstring(L, 1);
  double hold = lua_gettop(L) >= 2 ? luaL_checknumber(L, 2) : 0;
  uint8_t hat = 0xff;
  for (unsigned k = 0; k < sizeof HATS / sizeof HATS[0]; k++)
    if (!strcasecmp(d, HATS[k].name)) hat = HATS[k].hat;
  if (hat == 0xff) return luaL_error(L, "unknown hat '%s'", d);
  SDL_LockMutex(s->lock);
  s->input.hat = hat;
  SDL_UnlockMutex(s->lock);
  if (hold > 0) {
    nap(L, s, hold);
    SDL_LockMutex(s->lock);
    s->input.hat = SV_HAT_CENTER;
    SDL_UnlockMutex(s->lock);
    nap(L, s, 0.05);
  }
  return 0;
}

// stick("left", x, y) / stick("right", x, y, hold_s)
static int l_stick(lua_State *L)
{
  struct script *s = self(L);
  const char *which = luaL_checkstring(L, 1);
  double fx = luaL_checknumber(L, 2), fy = luaL_checknumber(L, 3);
  double hold = lua_gettop(L) >= 4 ? luaL_checknumber(L, 4) : 0;
  int16_t x = (int16_t)(fx * 32767.0);
  int16_t y = (int16_t)(fy * 32767.0);
  int is_left = !strcasecmp(which, "left") || !strcasecmp(which, "l");
  int is_right = !strcasecmp(which, "right") || !strcasecmp(which, "r");
  if (!is_left && !is_right) return luaL_error(L, "stick: expected 'left' or 'right'");
  SDL_LockMutex(s->lock);
  if (is_left) { s->input.lx = x; s->input.ly = y; }
  else { s->input.rx = x; s->input.ry = y; }
  SDL_UnlockMutex(s->lock);
  if (hold > 0) {
    nap(L, s, hold);
    SDL_LockMutex(s->lock);
    if (is_left) { s->input.lx = 0; s->input.ly = 0; }
    else { s->input.rx = 0; s->input.ry = 0; }
    SDL_UnlockMutex(s->lock);
    nap(L, s, 0.05);
  }
  return 0;
}

// type("hello\n")
static int l_type(lua_State *L)
{
  struct script *s = self(L);
  const char *str = luaL_checkstring(L, 1);
  SDL_LockMutex(s->lock);
  size_t cap = sizeof s->textq - s->textq_len;
  size_t n = strlen(str);
  if (n > cap) n = cap;
  memcpy(s->textq + s->textq_len, str, n);
  s->textq_len += n;
  SDL_UnlockMutex(s->lock);
  return 0;
}

// Special keys on the keyboard interface: key("enter"), key("esc"), key("up"), etc.
static const struct { const char *name; uint8_t byte; } KEYS[] = {
  {"enter", '\n'}, {"return", '\n'}, {"esc", 0x1b}, {"escape", 0x1b},
  {"backspace", '\b'}, {"tab", '\t'}, {"capslock", 0x11}, {"caps", 0x11},
  {"up", 0x12}, {"down", 0x13}, {"left", 0x14}, {"right", 0x15},
};

static int l_key(lua_State *L)
{
  struct script *s = self(L);
  const char *n = luaL_checkstring(L, 1);
  uint8_t byte = 0;
  for (unsigned k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++)
    if (!strcasecmp(n, KEYS[k].name)) {
      byte = KEYS[k].byte;
      break;
    }
  if (!byte) return luaL_error(L, "unknown key '%s'", n);
  SDL_LockMutex(s->lock);
  if (s->textq_len < sizeof s->textq) s->textq[s->textq_len++] = (char)byte;
  SDL_UnlockMutex(s->lock);
  return 0;
}

// capslock(0) ensures Caps Lock is off; capslock(1) ensures it's on; capslock()
// returns the current state (true/false, or nil if not yet reported by Pico).
static int l_capslock(lua_State *L)
{
  struct script *s = self(L);
  int leds = atomic_load(&s->leds);
  if (lua_gettop(L) == 0) {
    if (leds < 0) lua_pushnil(L);
    else lua_pushboolean(L, (leds & SV_LED_CAPSLOCK) != 0);
    return 1;
  }
  int want = lua_toboolean(L, 1);
  // Wait up to 1 s for a link LED report so we don't toggle blindly.
  uint32_t deadline = SDL_GetTicks() + 1000;
  while (leds < 0 && (int32_t)(deadline - SDL_GetTicks()) > 0) {
    nap(L, s, 0.05);
    leds = atomic_load(&s->leds);
  }
  if (leds < 0) {
    logf_(s, "capslock: host LED status unknown, toggling once");
    SDL_LockMutex(s->lock);
    if (s->textq_len < sizeof s->textq) s->textq[s->textq_len++] = 0x11;
    SDL_UnlockMutex(s->lock);
    return 0;
  }
  int cur = (leds & SV_LED_CAPSLOCK) != 0;
  if (cur != want) {
    logf_(s, "capslock: %s -> %s", cur ? "ON" : "off", want ? "ON" : "off");
    SDL_LockMutex(s->lock);
    if (s->textq_len < sizeof s->textq) s->textq[s->textq_len++] = 0x11;
    SDL_UnlockMutex(s->lock);
    // Give the toggle a moment to round-trip through the Switch -> Pico -> link.
    nap(L, s, 0.15);
  }
  return 0;
}

// text() -> latest raw OCR text (lower-case, collapsed whitespace)
static int l_text(lua_State *L)
{
  char buf[8192];
  fresh_text(L, self(L), SDL_GetTicks(), buf, sizeof buf);
  lua_pushstring(L, buf);
  return 1;
}

// screen() -> current screen label under the rules
static int l_screen(lua_State *L)
{
  struct script *s = self(L);
  char buf[8192];
  fresh_text(L, s, SDL_GetTicks(), buf, sizeof buf);
  lua_pushstring(L, classify(s, buf));
  return 1;
}

// wait("screen_label" [, timeout_s]) -> true or false
// wait({"label1", "label2"} [, timeout_s]) -> matched label or nil
static int l_wait(lua_State *L)
{
  struct script *s = self(L);
  double timeout = 10.0;
  int top = lua_gettop(L);
  if (top >= 2 && lua_type(L, top) == LUA_TNUMBER) timeout = lua_tonumber(L, top);

  uint32_t end = SDL_GetTicks() + (uint32_t)(timeout * 1000);
  char buf[8192];
  while ((int32_t)(end - SDL_GetTicks()) > 0) {
    uint32_t req = SDL_GetTicks();
    fresh_text(L, s, req, buf, sizeof buf);
    const char *label = classify(s, buf);

    if (lua_type(L, 1) == LUA_TSTRING) {
      if (!strcmp(label, lua_tostring(L, 1))) { lua_pushboolean(L, 1); return 1; }
    } else if (lua_type(L, 1) == LUA_TTABLE) {
      int len = (int)lua_rawlen(L, 1);
      for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, 1, i);
        const char *cand = lua_tostring(L, -1);
        int hit = cand && !strcmp(label, cand);
        lua_pop(L, 1);
        if (hit) { lua_pushstring(L, label); return 1; }
      }
    }
    nap(L, s, 0.05);
  }
  if (lua_type(L, 1) == LUA_TTABLE) lua_pushnil(L);
  else lua_pushboolean(L, 0);
  return 1;
}

// find("word") -> true if word is on screen right now
// find({"word1", "word2"}) -> true if ALL words appear on screen
static int l_find(lua_State *L)
{
  struct script *s = self(L);
  char buf[8192], squashed[8192];
  fresh_text(L, s, SDL_GetTicks(), buf, sizeof buf);
  squash(buf, squashed, sizeof squashed);

  int ok = 1;
  SDL_LockMutex(s->lock);
  s->nhl = 0;
  if (lua_type(L, 1) == LUA_TSTRING) {
    char word[64];
    squash(lua_tostring(L, 1), word, sizeof word);
    for (char *p = word; *p; p++) *p = (char)tolower((unsigned char)*p);
    ok = strstr(squashed, word) != NULL;
    if (ok) { snprintf(s->hl[0], sizeof s->hl[0], "%s", word); s->nhl = 1; }
  } else if (lua_type(L, 1) == LUA_TTABLE) {
    int len = (int)lua_rawlen(L, 1);
    for (int i = 1; i <= len; i++) {
      lua_rawgeti(L, 1, i);
      char word[64];
      squash(lua_tostring(L, -1), word, sizeof word);
      for (char *p = word; *p; p++) *p = (char)tolower((unsigned char)*p);
      lua_pop(L, 1);
      if (!strstr(squashed, word)) ok = 0;
      if (s->nhl < MAX_RULE_WORDS) snprintf(s->hl[s->nhl++], sizeof s->hl[0], "%s", word);
    }
  }
  if (!ok) s->nhl = 0;
  SDL_UnlockMutex(s->lock);
  lua_pushboolean(L, ok);
  return 1;
}

// rule("home", "nintendo", "switch", "online")
static int l_rule(lua_State *L)
{
  struct script *s = self(L);
  int n = lua_gettop(L);
  if (n < 2) return luaL_error(L, "rule(label, word1, ...)");
  SDL_LockMutex(s->lock);
  if (s->nrules >= MAX_RULES) {
    SDL_UnlockMutex(s->lock);
    return luaL_error(L, "too many rules");
  }
  struct rule *r = &s->rules[s->nrules++];
  snprintf(r->label, sizeof r->label, "%s", luaL_checkstring(L, 1));
  r->nwords = 0;
  for (int i = 2; i <= n && r->nwords < MAX_RULE_WORDS; i++) {
    char word[64];
    squash(luaL_checkstring(L, i), word, sizeof word);
    for (char *p = word; *p; p++) *p = (char)tolower((unsigned char)*p);
    snprintf(r->words[r->nwords++], sizeof r->words[0], "%s", word);
  }
  SDL_UnlockMutex(s->lock);
  return 0;
}

// rules{ home = {"nintendo", "switch"}, user_picker = "who is playing" }
static int l_rules(lua_State *L)
{
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushnil(L);
  while (lua_next(L, 1)) {
    const char *label = lua_tostring(L, -2);
    if (!label) { lua_pop(L, 1); continue; }
    lua_getglobal(L, "rule");
    lua_pushstring(L, label);
    if (lua_type(L, -3) == LUA_TTABLE) {
      int len = (int)lua_rawlen(L, -3);
      for (int i = 1; i <= len; i++) lua_rawgeti(L, -3 - i, i);
      lua_call(L, len + 1, 0);
    } else {
      lua_pushvalue(L, -3);
      lua_call(L, 2, 0);
    }
    lua_pop(L, 1);
  }
  return 0;
}

// region(x, y, w, h) in 0..1 of the frame; region() = whole screen.
static int l_region(lua_State *L)
{
  struct script *s = self(L);
  if (lua_gettop(L) == 0) ocr_set_region(s->ocr, 0, 0, 0, 0);
  else ocr_set_region(s->ocr, (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                      (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
  return 0;
}

static void stop_hook(lua_State *L, lua_Debug *ar)
{
  (void)ar;
  if (atomic_load(&self(L)->stop)) luaL_error(L, "stopped");
}

/* ---------------- thread ---------------- */

static void clear_input(struct script *s)
{
  SDL_LockMutex(s->lock);
  memset(&s->input, 0, sizeof s->input);
  s->input.hat = SV_HAT_CENTER;
  SDL_UnlockMutex(s->lock);
}

static int SDLCALL script_thread(void *arg)
{
  struct script *s = arg;
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  lua_pushlightuserdata(L, s);
  lua_setfield(L, LUA_REGISTRYINDEX, "sv_script");
  static const luaL_Reg api[] = {
    {"log", l_log}, {"sleep", l_sleep}, {"tap", l_tap}, {"press", l_press},
    {"release", l_release}, {"dpad", l_dpad}, {"stick", l_stick}, {"type", l_type},
    {"text", l_text}, {"screen", l_screen}, {"wait", l_wait}, {"find", l_find},
    {"rule", l_rule}, {"rules", l_rules}, {"region", l_region}, {"key", l_key},
    {"capslock", l_capslock}, {NULL, NULL},
  };
  for (const luaL_Reg *r = api; r->name; r++) lua_register(L, r->name, r->func);
  lua_sethook(L, stop_hook, LUA_MASKCOUNT, 1000);

  logf_(s, "> %s", strrchr(s->path, '/') ? strrchr(s->path, '/') + 1 : s->path);
  uint32_t t0 = SDL_GetTicks();
  int rc = luaL_dofile(L, s->path);
  if (rc != LUA_OK) {
    const char *err = lua_tostring(L, -1);
    if (err && strstr(err, "stopped")) logf_(s, "stopped");
    else logf_(s, "error: %s", err ? err : "?");
  } else {
    logf_(s, "done (%.1f s)", (SDL_GetTicks() - t0) / 1000.0);
  }
  lua_close(L);
  clear_input(s);
  ocr_set_enabled(s->ocr, 0);
  ocr_set_region(s->ocr, 0, 0, 0, 0);
  atomic_store(&s->running, 0);
  return 0;
}

/* ---------------- public ---------------- */

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

void script_rescan(struct script *s)
{
  s->count = 0;
  DIR *d = opendir(s->dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) && s->count < MAX_SCRIPTS) {
    size_t n = strlen(e->d_name);
    if (n > 4 && !strcmp(e->d_name + n - 4, ".lua"))
      snprintf(s->names[s->count++], sizeof s->names[0], "%s", e->d_name);
  }
  closedir(d);
  qsort(s->names, (size_t)s->count, sizeof s->names[0], cmp_names);
}

struct script *script_create(struct ocr *ocr, const char *dir)
{
  struct script *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->ocr = ocr;
  snprintf(s->dir, sizeof s->dir, "%s", dir);
  s->lock = SDL_CreateMutex();
  s->input.hat = SV_HAT_CENTER;
  atomic_store(&s->leds, -1);
  script_rescan(s);
  return s;
}

void script_destroy(struct script *s)
{
  if (!s) return;
  script_stop(s);
  if (s->lock) SDL_DestroyMutex(s->lock);
  free(s);
}

int script_count(struct script *s) { return s->count; }
const char *script_name(struct script *s, int i) { return i >= 0 && i < s->count ? s->names[i] : ""; }

int script_start(struct script *s, int index)
{
  if (atomic_load(&s->running) || index < 0 || index >= s->count || !s->ocr) return -1;
  SDL_LockMutex(s->lock);
  snprintf(s->path, sizeof s->path, "%s/%s", s->dir, s->names[index]);
  s->nrules = 0;
  s->nhl = 0;
  s->screen[0] = 0;
  s->textq_len = 0;
  SDL_UnlockMutex(s->lock);
  clear_input(s);
  atomic_store(&s->stop, 0);
  atomic_store(&s->running, 1);
  ocr_set_enabled(s->ocr, 1);
  s->th = SDL_CreateThread(script_thread, "script", s);
  if (!s->th) {
    atomic_store(&s->running, 0);
    ocr_set_enabled(s->ocr, 0);
    return -1;
  }
  SDL_DetachThread(s->th);
  return 0;
}

void script_stop(struct script *s)
{
  if (!atomic_load(&s->running)) return;
  atomic_store(&s->stop, 1);
  for (int i = 0; i < 200 && atomic_load(&s->running); i++) sv_usleep(5000);
}

int script_running(struct script *s) { return atomic_load(&s->running); }

void script_get_input(struct script *s, struct link_state *out, int *active)
{
  SDL_LockMutex(s->lock);
  *out = s->input;
  SDL_UnlockMutex(s->lock);
  *active = atomic_load(&s->running);
}

int script_take_text(struct script *s, char *buf, size_t len)
{
  SDL_LockMutex(s->lock);
  size_t n = s->textq_len < len - 1 ? s->textq_len : len - 1;
  memcpy(buf, s->textq, n);
  buf[n] = 0;
  memmove(s->textq, s->textq + n, s->textq_len - n);
  s->textq_len -= n;
  SDL_UnlockMutex(s->lock);
  return n > 0;
}

void script_screen(struct script *s, char *buf, size_t len)
{
  SDL_LockMutex(s->lock);
  snprintf(buf, len, "%s", s->screen);
  SDL_UnlockMutex(s->lock);
}

int script_log(struct script *s, char lines[][SCRIPT_LOG_COLS], int max)
{
  SDL_LockMutex(s->lock);
  int n = s->log_count < max ? s->log_count : max;
  int start = s->log_head + s->log_count - n;
  for (int i = 0; i < n; i++)
    memcpy(lines[i], s->log[(start + i) % SCRIPT_LOG_LINES], SCRIPT_LOG_COLS);
  SDL_UnlockMutex(s->lock);
  return n;
}

int script_highlights(struct script *s, char words[][64], int max)
{
  SDL_LockMutex(s->lock);
  int n = s->nhl < max ? s->nhl : max;
  memcpy(words, s->hl, sizeof words[0] * (size_t)n);
  SDL_UnlockMutex(s->lock);
  return n;
}

void script_set_leds(struct script *s, int leds) { atomic_store(&s->leds, leds); }
