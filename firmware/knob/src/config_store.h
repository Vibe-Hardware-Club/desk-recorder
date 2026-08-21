#pragma once
/*
 * CONFIG THAT LIVES ON THE DEVICE, NOT IN THE BINARY
 * ============================================================
 * WiFi, the ingest host and the bearer token used to be #defines in a
 * gitignored config.h, which meant every person who wanted one of these had to
 * install a C++ toolchain and compile their own. That is the single biggest
 * reason a project like this goes unused.
 *
 * So they live in NVS instead, written over the USB cable by the installer
 * after flashing. One published binary works for everybody, carries nobody's
 * credentials, and can be reconfigured later without a rebuild.
 *
 * THE RULE: values go in, they never come back out. STATUS reports whether a
 * key is set and how long it is, and that is all. A device on a desk that will
 * print its own bearer token to anyone with a cable is not a device we ship.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

struct Cfg {
  char ssid[33];    // 32 is the 802.11 maximum
  char pass[65];    // 64 is the WPA2 maximum
  char token[129];  // ours is 64 hex; headroom for whatever a user generates
  char host[97];    // "<project>.supabase.co", no scheme, no path
};

static Cfg cfg = {{0}, {0}, {0}, {0}};
static Preferences prefs;

static bool cfg_is_set() {
  return cfg.ssid[0] && cfg.token[0] && cfg.host[0];
}

static void cfg_load() {
  prefs.begin("knobcfg", true);   // read-only
  prefs.getString("ssid",  cfg.ssid,  sizeof(cfg.ssid));
  prefs.getString("pass",  cfg.pass,  sizeof(cfg.pass));
  prefs.getString("token", cfg.token, sizeof(cfg.token));
  prefs.getString("host",  cfg.host,  sizeof(cfg.host));
  prefs.end();
}

static void cfg_save() {
  prefs.begin("knobcfg", false);
  prefs.putString("ssid",  cfg.ssid);
  prefs.putString("pass",  cfg.pass);
  prefs.putString("token", cfg.token);
  prefs.putString("host",  cfg.host);
  prefs.end();
}

static void cfg_clear() {
  prefs.begin("knobcfg", false);
  prefs.clear();
  prefs.end();
  memset(&cfg, 0, sizeof(cfg));
}

/* Lengths only, never values. The installer parses these lines to confirm a
 * write landed, and a human reading the log learns nothing worth stealing. */
static void cfg_status() {
  Serial.printf("STATUS ssid %s %u\n",  cfg.ssid[0]  ? "set" : "unset", (unsigned)strlen(cfg.ssid));
  Serial.printf("STATUS pass %s %u\n",  cfg.pass[0]  ? "set" : "unset", (unsigned)strlen(cfg.pass));
  Serial.printf("STATUS token %s %u\n", cfg.token[0] ? "set" : "unset", (unsigned)strlen(cfg.token));
  Serial.printf("STATUS host %s %u\n",  cfg.host[0]  ? "set" : "unset", (unsigned)strlen(cfg.host));
  Serial.printf("STATUS ready %s\n", cfg_is_set() ? "yes" : "no");
}

/*
 * Returns true if the line was a config command and has been dealt with, so
 * the caller knows not to treat it as anything else.
 *
 * Grammar, one command per line:
 *   SET <ssid|pass|token|host> <value>   value is the rest of the line, verbatim,
 *                                        spaces included (WiFi passwords have them)
 *   SAVE                                 persist and report
 *   STATUS                               what is set, never what it is
 *   CLEAR                                forget everything
 */
static bool cfg_handle_line(char *line) {
  // Serial terminals send CRLF; a trailing \r inside a WiFi password is a
  // genuinely horrible afternoon, so it comes off here rather than later.
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;

  if (!strcmp(line, "STATUS")) { cfg_status(); return true; }

  if (!strcmp(line, "SAVE")) {
    cfg_save();
    Serial.println(cfg_is_set() ? "OK saved" : "ERR incomplete");
    cfg_status();
    return true;
  }

  if (!strcmp(line, "CLEAR")) {
    cfg_clear();
    Serial.println("OK cleared");
    return true;
  }

  if (strncmp(line, "SET ", 4)) return false;

  char *key = line + 4;
  char *sp  = strchr(key, ' ');
  if (!sp) { Serial.println("ERR usage: SET <key> <value>"); return true; }
  *sp = 0;
  const char *val = sp + 1;

  char *dst = NULL;
  size_t cap = 0;
  if      (!strcmp(key, "ssid"))  { dst = cfg.ssid;  cap = sizeof(cfg.ssid); }
  else if (!strcmp(key, "pass"))  { dst = cfg.pass;  cap = sizeof(cfg.pass); }
  else if (!strcmp(key, "token")) { dst = cfg.token; cap = sizeof(cfg.token); }
  else if (!strcmp(key, "host"))  { dst = cfg.host;  cap = sizeof(cfg.host); }
  else { Serial.printf("ERR unknown key %s\n", key); return true; }

  // Truncation here would be a WiFi password that silently does not work, so
  // it is refused loudly instead.
  if (strlen(val) >= cap) {
    Serial.printf("ERR %s too long, max %u\n", key, (unsigned)(cap - 1));
    return true;
  }
  snprintf(dst, cap, "%s", val);
  Serial.printf("OK %s %u\n", key, (unsigned)strlen(dst));
  return true;
}
