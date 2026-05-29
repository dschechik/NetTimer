/*
 * NetTimer - Network-Connected Electrical Timer for ESP8266 ESP-01S
 *
 * Hardware: ESP8266 ESP-01S + Relay Module
 * (e.g. HiLetgo ESP-01S Relay Module)
 *
 * Features:
 *  - 10 configurable timer events
 *  - On/Off relay control
 *  - Single-instance or day-of-week repeating events
 *  - Specific time or sunset-relative offset scheduling
 *  - AP mode for initial configuration
 *  - NTP time sync
 *  - Sunset calculation by lat/lon
 *  - JSON API
 *
 * Dependencies (install via Arduino Library Manager):
 *  - ESP8266WiFi        (bundled with ESP8266 Arduino core)
 *  - ESP8266WebServer   (bundled)
 *  - ESP8266mDNS        (bundled)
 *  - Preferences        (bundled)
 *  - Arduino_JSON       (Arduino_JSON by Arduino)
 *
 * NTP  : ESP8266 SDK built-in configTime() / time()
 * Store: Preferences library (key-value, wear-levelled flash)
 *
 * Relay pin: GPIO0 (active LOW on the HiLetgo module)
 * LED   pin: GPIO2 (built-in, active LOW)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <coredecls.h>      // settimeofday_cb()
#include <Preferences.h>
#include <Arduino_JSON.h>
#include <time.h>
#include <math.h>

// ─── Pin definitions ──────────────────────────────────────────────────────────
#define RELAY_PIN  0
#define LED_PIN    2

// ─── Event structure ──────────────────────────────────────────────────────────
struct TimerEvent {
  bool     enabled;
  bool     turnOn;
  bool     useSunset;
  bool     repeat;
  uint8_t  days;          // bitmask bit0=Sun…bit6=Sat
  int16_t  offsetMinutes; // abs time: minutes since midnight; sunset: offset in minutes
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
  bool     fired;
  uint8_t  _pad[1];
};

#define MAX_EVENTS 10

// ─── Globals ──────────────────────────────────────────────────────────────────
TimerEvent events[MAX_EVENTS];
char       cfgSSID[33] = "";
char       cfgPass[65] = "";
float      cfgLat      = 32.0853f;
float      cfgLon      = 34.7818f;
char       cfgTz[48]   = "IST-2IDT,M3.4.4/26,M10.5.0";  // Jerusalem default
char       cfgName[33] = "";                              // user-visible device name

bool       configured  = false;
bool       relayState  = false;
bool       ntpSynced   = false;
int        sunsetHour  = 19;
int        sunsetMin   = 30;
bool       sunsetValid = false;

Preferences      prefs;
ESP8266WebServer server(80);

// ─── NTP callback ─────────────────────────────────────────────────────────────
void ntpSyncCb() { ntpSynced = true; Serial.println("NTP synced"); }

// ─── Preferences storage ──────────────────────────────────────────────────────
void saveConfig() {
  prefs.begin("cfg", false);
  // Use putBytes for credentials: putString on ESP8266 NVS can silently
  // corrupt strings containing special characters common in WiFi passwords.
  // Store strlen+1 (string + null terminator) not sizeof(buffer).
  // Storing the full buffer means getBytes() returns sizeof on reload,
  // causing the null-termination write to go one byte past the array end.
  prefs.putBytes("ssid", cfgSSID, strlen(cfgSSID) + 1);
  prefs.putBytes("pass", cfgPass, strlen(cfgPass) + 1);
  prefs.putFloat("lat",  cfgLat);
  prefs.putFloat("lon",  cfgLon);
  prefs.putBytes("tz",   cfgTz,   strlen(cfgTz)   + 1);
  prefs.putBytes("name", cfgName, strlen(cfgName) + 1);
  prefs.end();
}

bool loadConfig() {
  prefs.begin("cfg", true);
  size_t sLen = prefs.getBytes("ssid", cfgSSID, sizeof(cfgSSID));
  size_t pLen = prefs.getBytes("pass", cfgPass, sizeof(cfgPass));
  cfgLat  = prefs.getFloat("lat",  32.0853f);
  cfgLon  = prefs.getFloat("lon",  34.7818f);
  size_t tLen = prefs.getBytes("tz",   cfgTz,   sizeof(cfgTz)-1);
  size_t nLen = prefs.getBytes("name", cfgName, sizeof(cfgName)-1);
  prefs.end();
  if (sLen < sizeof(cfgSSID)) cfgSSID[sLen] = '\0'; else cfgSSID[sizeof(cfgSSID)-1] = '\0';
  if (pLen < sizeof(cfgPass)) cfgPass[pLen] = '\0'; else cfgPass[sizeof(cfgPass)-1] = '\0';
  if (tLen > 0) cfgTz[tLen]     = '\0'; else cfgTz[sizeof(cfgTz)-1]     = '\0';
  if (nLen > 0) cfgName[nLen]   = '\0'; else cfgName[sizeof(cfgName)-1] = '\0';
  return sLen > 0 && cfgSSID[0] != '\0';
}

void saveEvent(int i) {
  char ns[4]; snprintf(ns, sizeof(ns), "ev%d", i);
  prefs.begin(ns, false);
  prefs.putBytes("d", &events[i], sizeof(TimerEvent));
  prefs.end();
}

void loadEvents() {
  for (int i = 0; i < MAX_EVENTS; i++) {
    char ns[4]; snprintf(ns, sizeof(ns), "ev%d", i);
    prefs.begin(ns, true);
    size_t n = prefs.getBytes("d", &events[i], sizeof(TimerEvent));
    prefs.end();
    if (n != sizeof(TimerEvent)) memset(&events[i], 0, sizeof(TimerEvent));
    else if (events[i].offsetMinutes < -720 || events[i].offsetMinutes > 1439)
      events[i].enabled = false;
  }
}

void clearAll() {
  prefs.begin("cfg", false); prefs.clear(); prefs.end();
  for (int i = 0; i < MAX_EVENTS; i++) {
    char ns[4]; snprintf(ns, sizeof(ns), "ev%d", i);
    prefs.begin(ns, false); prefs.clear(); prefs.end();
  }
}

// ─── Sunset calculation (NOAA simplified algorithm) ──────────────
// Returns local sunset as minutes since midnight, or -1 (polar day/night).
// IMPORTANT: use acos(cosH)/15 for sunset, NOT (360-acos(cosH))/15.
// The (360-acos) form only works for western longitudes. For eastern
// longitudes the 18h seed already falls after solar noon, so the complement
// selects the wrong (sunrise) crossing.
int calcSunset(int year, int month, int day, float lat, float lon) {
  int N1 = (int)floor(275.0 * month / 9.0);
  int N2 = (int)floor((month + 9.0) / 12.0);
  int N3 = (int)(1 + floor((year - 4.0 * floor(year / 4.0) + 2.0) / 3.0));
  int N  = N1 - (N2 * N3) + day - 30;
  double lngHour = lon / 15.0;
  double t = N + ((18.0 - lngHour) / 24.0);
  double M = (0.9856 * t) - 3.289;
  double L = M + (1.916 * sin(M * DEG_TO_RAD))
               + (0.020 * sin(2.0 * M * DEG_TO_RAD)) + 282.634;
  while (L <   0.0) L += 360.0;
  while (L >= 360.0) L -= 360.0;
  double RA = atan(0.91764 * tan(L * DEG_TO_RAD)) * RAD_TO_DEG;
  while (RA <   0.0) RA += 360.0;
  while (RA >= 360.0) RA -= 360.0;
  RA = RA + (floor(L / 90.0) * 90.0 - floor(RA / 90.0) * 90.0);
  RA = RA / 15.0;
  double sinDec = 0.39782 * sin(L * DEG_TO_RAD);
  double cosDec = cos(asin(sinDec));
  double cosH = (cos(90.833 * DEG_TO_RAD)
                - (sinDec * sin((double)lat * DEG_TO_RAD)))
              / (cosDec  *  cos((double)lat * DEG_TO_RAD));
  if (cosH >  1.0) return -1;
  if (cosH < -1.0) return -1;
  // Sunset hour angle: acos gives the correct evening crossing for all longitudes
  double H  = acos(cosH) * RAD_TO_DEG / 15.0;
  double T  = H + RA - (0.06571 * t) - 6.622;
  double UT = T - lngHour;
  while (UT <   0.0) UT += 24.0;
  while (UT >= 24.0) UT -= 24.0;
  // Derive the current UTC offset from the system timezone (handles DST)
  time_t now = time(nullptr);
  struct tm utcTm, locTm;
  gmtime_r(&now, &utcTm);
  localtime_r(&now, &locTm);
  int tzOffsetMin = (int)(mktime(&locTm) - mktime(&utcTm)) / 60;
  double local = UT + (tzOffsetMin / 60.0);
  while (local <   0.0) local += 24.0;
  while (local >= 24.0) local -= 24.0;
  return (int)round(local * 60.0);
}

// ─── Relay ────────────────────────────────────────────────────────────────────
void setRelay(bool on) {
  relayState = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  digitalWrite(LED_PIN,   on ? LOW : HIGH);
}

// ─── Time ─────────────────────────────────────────────────────────────────────
// mDNS hostname helper: lowercase alnum + hyphens, falls back to "net-timer"
void makeMdnsHostname(char* out, size_t maxLen) {
  const char* s = cfgName[0] ? cfgName : "net-timer";
  size_t j = 0;
  for (size_t i = 0; s[i] && j < maxLen - 1; i++) {
    char c = tolower((unsigned char)s[i]);
    if (isalnum((unsigned char)c))    out[j++] = c;
    else if (c == ' ' || c == '-') out[j++] = '-';
  }
  while (j > 0 && out[j-1] == '-') j--;
  out[j] = '\0';
  if (j == 0) strncpy(out, "net-timer", maxLen);
}

void applyTimezone() {
  setenv("TZ", cfgTz, 1);
  tzset();
}

struct tm getLocalTime() {
  time_t now = time(nullptr); struct tm t; localtime_r(&now, &t); return t;
}

// ─── Event evaluation ─────────────────────────────────────────────────────────
void checkEvents() {
  struct tm t = getLocalTime();
  int nowMin = t.tm_hour*60 + t.tm_min;
  int nowSec = t.tm_sec;
  if (nowMin == 0 && nowSec < 30) {
    int sm = calcSunset(t.tm_year+1900, t.tm_mon+1, t.tm_mday, cfgLat, cfgLon);
    if (sm >= 0) { sunsetHour = sm/60; sunsetMin = sm%60; sunsetValid = true; }
  }
  for (int i = 0; i < MAX_EVENTS; i++) {
    TimerEvent& ev = events[i];
    if (!ev.enabled) continue;
    int trig;
    if (ev.useSunset) {
      if (!sunsetValid) continue;
      trig = sunsetHour*60 + sunsetMin + ev.offsetMinutes;
      if (trig < 0) trig += 1440; if (trig >= 1440) trig -= 1440;
    } else {
      trig = ev.offsetMinutes;
    }
    if (nowMin != trig || nowSec > 59) continue;
    if (ev.repeat) {
      if (!(ev.days & (1 << t.tm_wday))) continue;
    } else {
      if (ev.fired) continue;
      if ((t.tm_year+1900) != ev.year || (t.tm_mon+1) != ev.month || t.tm_mday != ev.day) continue;
    }
    setRelay(ev.turnOn);
    if (!ev.repeat) { ev.fired = true; ev.enabled = false; saveEvent(i); }
  }
}

// ─── HTML streaming helpers ───────────────────────────────────────────────────
// Pages are streamed in small chunks so no single String grows large.
// Never use F() in a String += chain — read PROGMEM via sendContent() directly.

static const char PAGE_CSS[] PROGMEM =
  "<style>"
  "*{box-sizing:border-box;margin:0;padding:0}"
  "body{font-family:monospace;background:#0d1117;color:#e6edf3;padding:16px}"
  "h1{font-size:1.3em;color:#58a6ff;border-bottom:1px solid #30363d;padding-bottom:8px;margin-bottom:16px}"
  "h2{font-size:1.05em;color:#79c0ff;margin:14px 0 6px}"
  "a{color:#58a6ff;text-decoration:none}"
  "nav{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:8px;margin-bottom:18px;display:flex;gap:10px;flex-wrap:wrap}"
  "nav a{padding:3px 9px;border-radius:4px;border:1px solid #30363d;font-size:.85em}"
  "label{display:block;margin:7px 0 2px;font-size:.85em;color:#8b949e}"
  "input,select{width:100%;padding:6px 9px;background:#161b22;border:1px solid #30363d;border-radius:5px;color:#e6edf3;font-size:.9em;margin-bottom:5px}"
  "input[type=checkbox]{width:auto;margin-right:5px}"
  ".row{display:flex;gap:8px;flex-wrap:wrap}"
  ".row>div{flex:1;min-width:110px}"
  "button{background:#238636;color:#fff;border:none;padding:8px 14px;border-radius:5px;cursor:pointer;font-size:.9em;width:100%;margin-top:5px}"
  "button:hover{background:#2ea043}"
  ".bd{background:#b62324}.bd:hover{background:#da3633}"
  ".card{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:12px;margin-bottom:10px}"
  ".eh{display:flex;justify-content:space-between;align-items:center}"
  ".badge{font-size:.75em;padding:2px 7px;border-radius:10px;background:#21262d;border:1px solid #30363d}"
  ".on{color:#3fb950;border-color:#3fb950}"
  ".off{color:#f85149;border-color:#f85149}"
  ".sr{font-size:.82em;color:#8b949e;margin-top:4px}"
  ".ok{padding:9px;border-radius:5px;margin-bottom:10px;background:#0d4429;border:1px solid #3fb950;color:#3fb950;font-size:.9em}"
  ".err{padding:9px;border-radius:5px;margin-bottom:10px;background:#3d0000;border:1px solid #f85149;color:#f85149;font-size:.9em}"
  ".sep{border:none;border-top:1px solid #30363d;margin:14px 0}"
  ".ron{color:#3fb950;font-weight:bold}.roff{color:#8b949e}"
  "</style>";

static const char PAGE_NAV[] PROGMEM =
  "<nav>"
  "<a href='/'>Status</a>"
  "<a href='/set'>Events</a>"
  "<a href='/config'>Config</a>"
  "<a href='/reset'>Reset</a>"
  "</nav>";

// Begin a chunked HTML response
void htmlStart(const char* title) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  // doctype + head open
  server.sendContent("<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>");
  // Include device name in <title> if set
  if (cfgName[0]) { server.sendContent(cfgName); server.sendContent(" - "); }
  server.sendContent("NetTimer</title>");
  // CSS from PROGMEM
  server.sendContent_P(PAGE_CSS);
  // head close + body open + h1
  server.sendContent("</head><body><h1>");
  if (cfgName[0]) { server.sendContent(cfgName); server.sendContent(" &mdash; "); }
  server.sendContent("NetTimer");
  if (title && title[0]) { server.sendContent(" &mdash; "); server.sendContent(title); }
  server.sendContent("</h1>");
  // nav from PROGMEM
  server.sendContent_P(PAGE_NAV);
}

void htmlEnd() { server.sendContent("</body></html>"); server.sendContent(""); }

// ─── Web: Status page ─────────────────────────────────────────────────────────
void handleRoot() {
  struct tm t = getLocalTime();
  char tbuf[20], dbuf[30];
  strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &t);
  strftime(dbuf, sizeof(dbuf), "%A %d %b %Y", &t);
  char tmp[120];

  htmlStart("Status");

  server.sendContent("<div class='card'>");
  snprintf(tmp, sizeof(tmp), "<p><b>Time:</b> %s</p><p><b>Date:</b> %s</p>", tbuf, dbuf);
  server.sendContent(tmp);

  snprintf(tmp, sizeof(tmp), "<p><b>Relay:</b> <span class='r%s'>%s</span></p>",
           relayState ? "on'>ON" : "off'>OFF", "");
  // simpler:
  server.sendContent("<p><b>Relay:</b> <span class='");
  server.sendContent(relayState ? "ron'>ON" : "roff'>OFF");
  server.sendContent("</span></p>");

  if (sunsetValid) {
    snprintf(tmp, sizeof(tmp), "<p><b>Sunset:</b> %02d:%02d</p>", sunsetHour, sunsetMin);
    server.sendContent(tmp);
  }
  snprintf(tmp, sizeof(tmp), "<p><b>Location:</b> %.4f / %.4f</p>", cfgLat, cfgLon);
  server.sendContent(tmp);
  server.sendContent("<p><b>NTP:</b> ");
  server.sendContent(ntpSynced ? "Synced" : "Pending");
  server.sendContent("</p></div>");

  server.sendContent(
    "<form method='POST' action='/relay'><div class='row'>"
    "<div><button name='state' value='1'>Turn ON</button></div>"
    "<div><button class='bd' name='state' value='0'>Turn OFF</button></div>"
    "</div></form>");

  htmlEnd();
}

void handleRelayPost() {
  if (server.hasArg("state")) setRelay(server.arg("state") == "1");
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

// ─── Web: Events page ─────────────────────────────────────────────────────────
void sendEventDescription(int i) {
  TimerEvent& ev = events[i];
  char buf[40];
  server.sendContent(ev.turnOn ? "<span class='badge on'>ON</span> " : "<span class='badge off'>OFF</span> ");
  if (ev.useSunset) {
    if (ev.offsetMinutes == 0) server.sendContent("at sunset");
    else {
      snprintf(buf, sizeof(buf), "%+d min from sunset", ev.offsetMinutes);
      server.sendContent(buf);
    }
  } else {
    snprintf(buf, sizeof(buf), "at %02d:%02d", ev.offsetMinutes/60, ev.offsetMinutes%60);
    server.sendContent(buf);
  }
  if (ev.repeat) {
    const char* dn[] = {"Su","Mo","Tu","We","Th","Fr","Sa"};
    server.sendContent(" every [");
    bool first = true;
    for (int d = 0; d < 7; d++) {
      if (ev.days & (1<<d)) {
        if (!first) server.sendContent(",");
        server.sendContent(dn[d]);
        first = false;
      }
    }
    server.sendContent("]");
  } else {
    snprintf(buf, sizeof(buf), " once %04d-%02d-%02d", ev.year, ev.month, ev.day);
    server.sendContent(buf);
    if (ev.fired) server.sendContent(" (fired)");
  }
}

void handleSet() {
  String msg = "";

  if (server.method() == HTTP_POST && server.hasArg("delete")) {
    int idx = server.arg("delete").toInt();
    if (idx >= 0 && idx < MAX_EVENTS) {
      memset(&events[idx], 0, sizeof(TimerEvent));
      saveEvent(idx);
      msg = "<div class='ok'>Event " + String(idx+1) + " deleted.</div>";
    }
  }

  if (server.method() == HTTP_POST && server.hasArg("action")) {
    int slot = -1;
    for (int i = 0; i < MAX_EVENTS; i++) if (!events[i].enabled) { slot=i; break; }
    if (slot < 0) {
      msg = "<div class='err'>All 10 slots in use.</div>";
    } else {
      TimerEvent& ev = events[slot];
      memset(&ev, 0, sizeof(TimerEvent));
      ev.enabled   = true;
      ev.turnOn    = (server.arg("action") == "1");
      ev.useSunset = (server.arg("timetype") == "sunset");
      ev.repeat    = (server.arg("sched") == "repeat");
      if (ev.useSunset) {
        ev.offsetMinutes = server.arg("sunoffset").toInt();
      } else {
        ev.offsetMinutes = server.arg("hour").toInt()*60 + server.arg("minute").toInt();
      }
      if (ev.repeat) {
        ev.days = 0;
        const char* dk[] = {"d0","d1","d2","d3","d4","d5","d6"};
        for (int d=0; d<7; d++) if (server.hasArg(dk[d])) ev.days |= (1<<d);
        if (!ev.days) ev.days = 0x7F;
      } else {
        ev.year  = server.arg("eyear").toInt();
        ev.month = server.arg("emonth").toInt();
        ev.day   = server.arg("eday").toInt();
      }
      saveEvent(slot);
      msg = "<div class='ok'>Event " + String(slot+1) + " saved.</div>";
    }
  }

  htmlStart("Events");
  if (msg.length()) server.sendContent(msg);

  server.sendContent("<h2>Current Events</h2>");
  int cnt = 0;
  char tmp[220];
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!events[i].enabled) continue;
    cnt++;
    server.sendContent("<div class='card'><div class='eh'>");
    snprintf(tmp, sizeof(tmp), "<b>Event %d</b>", i+1);
    server.sendContent(tmp);
    server.sendContent("<form method='POST' action='/set' style='margin:0'>");
    snprintf(tmp, sizeof(tmp),
      "<button class='bd' name='delete' value='%d' "
      "style='width:auto;padding:2px 8px;font-size:.8em'>Delete</button>", i);
    server.sendContent(tmp);
    server.sendContent("</form></div><div class='sr'>");
    sendEventDescription(i);
    server.sendContent("</div></div>");
  }
  if (!cnt) server.sendContent("<p style='color:#8b949e;font-size:.9em'>No events configured.</p>");

  server.sendContent(
    "<hr class='sep'><h2>Add Event</h2>"
    "<form method='POST' action='/set'>"
    "<div class='row'>"
    "<div><label>Action</label>"
    "<select name='action'>"
    "<option value='1'>Turn ON</option>"
    "<option value='0'>Turn OFF</option>"
    "</select></div>"
    "<div><label>Schedule</label>"
    "<select name='sched' onchange=\""
      "document.getElementById('rpt').style.display=this.value=='repeat'?'block':'none';"
      "document.getElementById('once').style.display=this.value=='once'?'block':'none'\">"
    "<option value='repeat'>Repeat</option>"
    "<option value='once'>One-time</option>"
    "</select></div></div>"
    "<div class='row'><div><label>Time type</label>"
    "<select name='timetype' onchange=\""
      "document.getElementById('abt').style.display=this.value=='abs'?'flex':'none';"
      "document.getElementById('snt').style.display=this.value=='sunset'?'block':'none'\">"
    "<option value='abs'>Specific time</option>"
    "<option value='sunset'>Sunset offset</option>"
    "</select></div></div>"
    "<div id='abt' class='row'>"
    "<div><label>Hour (0-23)</label><input type='number' name='hour' min='0' max='23' value='18'></div>"
    "<div><label>Minute</label><input type='number' name='minute' min='0' max='59' value='0'></div>"
    "</div>"
    "<div id='snt' style='display:none'>"
    "<label>Offset from sunset (min, negative=before)</label>"
    "<input type='number' name='sunoffset' value='0'>"
    "</div>"
    "<div id='rpt'><label>Days</label><div class='row'>"
    "<div><label><input type='checkbox' name='d0' checked>Sun</label></div>"
    "<div><label><input type='checkbox' name='d1' checked>Mon</label></div>"
    "<div><label><input type='checkbox' name='d2' checked>Tue</label></div>"
    "<div><label><input type='checkbox' name='d3' checked>Wed</label></div>"
    "<div><label><input type='checkbox' name='d4' checked>Thu</label></div>"
    "<div><label><input type='checkbox' name='d5' checked>Fri</label></div>"
    "<div><label><input type='checkbox' name='d6' checked>Sat</label></div>"
    "</div></div>");

  // One-shot date — needs current date so built dynamically
  struct tm t = getLocalTime();
  server.sendContent("<div id='once' style='display:none'><div class='row'>");
  snprintf(tmp, sizeof(tmp), "<div><label>Year</label>"
    "<input type='number' name='eyear' value='%d'></div>", t.tm_year+1900);
  server.sendContent(tmp);
  snprintf(tmp, sizeof(tmp), "<div><label>Month</label>"
    "<input type='number' name='emonth' min='1' max='12' value='%d'></div>", t.tm_mon+1);
  server.sendContent(tmp);
  snprintf(tmp, sizeof(tmp), "<div><label>Day</label>"
    "<input type='number' name='eday' min='1' max='31' value='%d'></div>", t.tm_mday);
  server.sendContent(tmp);
  server.sendContent("</div></div>");

  server.sendContent("<button type='submit'>Add Event</button></form>");
  htmlEnd();
}

// ─── Web: Config page ─────────────────────────────────────────────────────────
void handleConfig() {
  bool saved = false;
  if (server.method() == HTTP_POST) {
    server.arg("ssid").toCharArray(cfgSSID, sizeof(cfgSSID));
    server.arg("pass").toCharArray(cfgPass, sizeof(cfgPass));
    server.arg("name").toCharArray(cfgName, sizeof(cfgName));
    cfgLat = server.arg("lat").toFloat();
    cfgLon = server.arg("lon").toFloat();
    server.arg("tz").toCharArray(cfgTz, sizeof(cfgTz));
    saveConfig();
    saved = true;
  }

  htmlStart("Config");
  if (saved) server.sendContent(
    "<div class='ok'>Saved. Rebooting...</div>"
    "<script>setTimeout(()=>location='/',4000)</script>");

  // Build form in snprintf buffers so no sendContent() ever receives
  // an empty string (which terminates the chunked HTTP response).
  char tmp[220];
  server.sendContent("<form method='POST' action='/config'>");

  // Device name field
  snprintf(tmp, sizeof(tmp),
    "<label>Device Name (shown in page title)</label>"
    "<input type='text' name='name' maxlength='32' value='%s'>", cfgName);
  server.sendContent(tmp);

  // SSID value embedded in buffer so empty cfgSSID is safe
  snprintf(tmp, sizeof(tmp),
    "<label>WiFi SSID</label>"
    "<input type='text' name='ssid' maxlength='32' value='%s'>"
    "<label>WiFi Password</label>"
    "<input type='password' name='pass' maxlength='64'>",
    cfgSSID);
  server.sendContent(tmp);

  server.sendContent(
    "<hr class='sep'><h2>Location &amp; Timezone</h2>"
    "<p style='font-size:.8em;color:#8b949e'>Used for sunset and local time.</p>"
    "<div class='row'>");

  snprintf(tmp, sizeof(tmp),
    "<div><label>Latitude</label>"
    "<input type='number' step='0.0001' name='lat' value='%.4f'></div>"
    "<div><label>Longitude</label>"
    "<input type='number' step='0.0001' name='lon' value='%.4f'></div>"
    "</div>", cfgLat, cfgLon);
  server.sendContent(tmp);

  // Timezone dropdown - POSIX TZ strings with DST rules
  server.sendContent("<label>Timezone</label><select name='tz'>");
  static const char* const TZ_POSIX[] = {
    "HST10",
    "AKST9AKDT,M3.2.0,M11.1.0",
    "PST8PDT,M3.2.0,M11.1.0",
    "MST7MDT,M3.2.0,M11.1.0",
    "MST7",
    "CST6CDT,M3.2.0,M11.1.0",
    "EST5EDT,M3.2.0,M11.1.0",
    "AST4ADT,M3.2.0,M11.1.0",
    "BRT3",
    "GMT0",
    "GMT0BST,M3.5.0/1,M10.5.0",
    "CET-1CEST,M3.5.0,M10.5.0/3",
    "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "IST-2IDT,M3.4.4/26,M10.5.0",
    "MSK-3",
    "GST-4",
    "IST-5:30",
    "WIB-7",
    "CST-8",
    "JST-9",
    "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "NZST-12NZDT,M9.5.0,M4.1.0/3"
  };
  static const char* const TZ_LABEL[] = {
    "Hawaii (UTC-10)",
    "Alaska (UTC-9/-8)",
    "US Pacific (UTC-8/-7)",
    "US Mountain (UTC-7/-6)",
    "US Arizona (UTC-7, no DST)",
    "US Central (UTC-6/-5)",
    "US Eastern (UTC-5/-4)",
    "Atlantic / Halifax (UTC-4/-3)",
    "Brazil / Sao Paulo (UTC-3)",
    "UTC / GMT (UTC+0)",
    "UK / Ireland (UTC+0/+1)",
    "Central Europe (UTC+1/+2)",
    "Eastern Europe (UTC+2/+3)",
    "Israel (UTC+2/+3)",
    "Moscow (UTC+3)",
    "Gulf / Dubai (UTC+4)",
    "India (UTC+5:30)",
    "W. Indonesia / Bangkok (UTC+7)",
    "China / Singapore / HK (UTC+8)",
    "Japan / Korea (UTC+9)",
    "Australia Eastern (UTC+10/+11)",
    "New Zealand (UTC+12/+13)"
  };
  int tzCount = sizeof(TZ_POSIX) / sizeof(TZ_POSIX[0]);
  for (int z = 0; z < tzCount; z++) {
    server.sendContent("<option value='");
    server.sendContent(TZ_POSIX[z]);
    server.sendContent(strcmp(TZ_POSIX[z], cfgTz) == 0 ? "' selected>" : "'>");
    server.sendContent(TZ_LABEL[z]);
    server.sendContent("</option>");
  }
  server.sendContent("</select>");

  server.sendContent("<button type='submit'>Save &amp; Reboot</button></form>");
  htmlEnd();

  if (saved) { delay(3000); ESP.restart(); }
}

// ─── Web: Reset page ──────────────────────────────────────────────────────────
void handleReset() {
  if (server.method() == HTTP_POST && server.arg("confirm") == "yes") {
    clearAll();
    htmlStart("Reset");
    server.sendContent("<div class='ok'>Reset complete. Rebooting…</div>");
    htmlEnd();
    delay(2000);
    ESP.restart();
    return;
  }
  htmlStart("Reset");
  server.sendContent(
    "<div class='card'>"
    "<p style='color:#f85149;margin-bottom:10px'>"
    "This will erase all settings and events.</p>"
    "<form method='POST' action='/reset'>"
    "<input type='hidden' name='confirm' value='yes'>"
    "<button class='bd' type='submit'>Confirm Factory Reset</button>"
    "</form></div>");
  htmlEnd();
}

// ─── API helpers ──────────────────────────────────────────────────────────────
int findFreeSlot() {
  for (int i = 0; i < MAX_EVENTS; i++) if (!events[i].enabled) return i;
  return -1;
}

// ─── API: Add event ───────────────────────────────────────────────────────────
void handleApiAddEvent() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}"); return;
  }
  JSONVar doc = JSON.parse(server.arg("plain"));
  if (JSON.typeof(doc) == "undefined") {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int slot = findFreeSlot();
  if (slot < 0) {
    server.send(507, "application/json", "{\"error\":\"No free slots\"}"); return;
  }
  TimerEvent& ev = events[slot];
  memset(&ev, 0, sizeof(TimerEvent));
  ev.enabled   = true;
  ev.turnOn    = (String((const char*)doc["action"]) == "on");
  ev.useSunset = (String((const char*)doc["time_type"]) == "sunset");
  ev.repeat    = (String((const char*)doc["schedule"]) != "once");
  if (ev.useSunset) {
    ev.offsetMinutes = doc.hasOwnProperty("sun_offset") ? (int)doc["sun_offset"] : 0;
  } else {
    int h = doc.hasOwnProperty("hour")   ? (int)doc["hour"]   : 0;
    int m = doc.hasOwnProperty("minute") ? (int)doc["minute"] : 0;
    ev.offsetMinutes = h*60 + m;
  }
  if (ev.repeat) {
    ev.days = 0;
    if (JSON.typeof(doc["days"]) == "array")
      for (int d=0; d<(int)doc["days"].length(); d++) {
        int day=(int)doc["days"][d]; if(day>=0&&day<=6) ev.days|=(1<<day);
      }
    if (!ev.days) ev.days = 0x7F;
  } else {
    ev.year  = doc.hasOwnProperty("year")  ? (int)doc["year"]  : 0;
    ev.month = doc.hasOwnProperty("month") ? (int)doc["month"] : 0;
    ev.day   = doc.hasOwnProperty("day")   ? (int)doc["day"]   : 0;
    if (!ev.year || !ev.month || !ev.day) {
      server.send(400, "application/json", "{\"error\":\"Missing year/month/day\"}"); return;
    }
  }
  saveEvent(slot);
  char r[40]; snprintf(r, sizeof(r), "{\"ok\":true,\"slot\":%d}", slot);
  server.send(200, "application/json", r);
}

// ─── API: Delete event ────────────────────────────────────────────────────────
void handleApiDeleteEvent() {
  if (server.method() != HTTP_DELETE) {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}"); return;
  }
  JSONVar doc = JSON.parse(server.arg("plain"));
  if (JSON.typeof(doc) == "undefined") {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  if (!doc.hasOwnProperty("slot")) {
    server.send(400, "application/json", "{\"error\":\"Missing slot\"}"); return;
  }
  int slot = (int)doc["slot"];
  if (slot < 0 || slot >= MAX_EVENTS) {
    server.send(400, "application/json", "{\"error\":\"slot must be 0-9\"}"); return;
  }
  if (!events[slot].enabled) {
    server.send(404, "application/json", "{\"error\":\"Slot already empty\"}"); return;
  }
  memset(&events[slot], 0, sizeof(TimerEvent));
  saveEvent(slot);
  char r[40]; snprintf(r, sizeof(r), "{\"ok\":true,\"slot\":%d}", slot);
  server.send(200, "application/json", r);
}

// ─── API: Set relay state ─────────────────────────────────────────────────────
void handleApiRelay() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}"); return;
  }
  JSONVar doc = JSON.parse(server.arg("plain"));
  if (JSON.typeof(doc) == "undefined") {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  if (!doc.hasOwnProperty("state")) {
    server.send(400, "application/json", "{\"error\":\"Missing state\"}"); return;
  }
  String state = String((const char*)doc["state"]);
  if (state != "on" && state != "off") {
    server.send(400, "application/json", "{\"error\":\"state must be on or off\"}"); return;
  }
  setRelay(state == "on");
  char r[40]; snprintf(r, sizeof(r), "{\"ok\":true,\"relay\":\"%s\"}", state.c_str());
  server.send(200, "application/json", r);
}

// ─── API: Legacy one-time event ───────────────────────────────────────────────
void handleApiEvent() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"error\":\"Method Not Allowed\"}"); return;
  }
  JSONVar doc = JSON.parse(server.arg("plain"));
  if (JSON.typeof(doc) == "undefined") {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
  }
  int slot = findFreeSlot();
  if (slot < 0) {
    server.send(507, "application/json", "{\"error\":\"No free slots\"}"); return;
  }
  TimerEvent& ev = events[slot];
  memset(&ev, 0, sizeof(TimerEvent));
  ev.enabled       = true;
  ev.turnOn        = (String((const char*)doc["action"]) == "on");
  ev.year          = doc.hasOwnProperty("year")   ? (int)doc["year"]   : 0;
  ev.month         = doc.hasOwnProperty("month")  ? (int)doc["month"]  : 0;
  ev.day           = doc.hasOwnProperty("day")    ? (int)doc["day"]    : 0;
  ev.offsetMinutes = (doc.hasOwnProperty("hour")  ? (int)doc["hour"]  : 0) * 60
                   + (doc.hasOwnProperty("minute")? (int)doc["minute"]: 0);
  if (!ev.year || !ev.month || !ev.day) {
    server.send(400, "application/json", "{\"error\":\"Missing year/month/day\"}"); return;
  }
  saveEvent(slot);
  char r[40]; snprintf(r, sizeof(r), "{\"ok\":true,\"slot\":%d}", slot);
  server.send(200, "application/json", r);
}

// ─── API: Status ──────────────────────────────────────────────────────────────
void handleApiStatus() {
  struct tm t = getLocalTime();
  char tbuf[24]; strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &t);
  JSONVar doc;
  doc["name"]       = cfgName[0] ? cfgName : "NetTimer";
  doc["relay"]      = relayState;
  doc["ntp_synced"] = ntpSynced;
  doc["epoch"]      = (long)time(nullptr);
  doc["local_time"] = tbuf;
  if (sunsetValid) {
    char sun[8]; snprintf(sun, sizeof(sun), "%02d:%02d", sunsetHour, sunsetMin);
    doc["sunset"] = sun;
  }
  JSONVar arr; int cnt = 0;
  for (int i = 0; i < MAX_EVENTS; i++) {
    if (!events[i].enabled) continue;
    JSONVar o;
    o["slot"]       = i;
    o["action"]     = events[i].turnOn ? "on" : "off";
    o["use_sunset"] = events[i].useSunset;
    o["repeat"]     = events[i].repeat;
    o["offset_min"] = events[i].offsetMinutes;
    arr[cnt++]      = o;
  }
  doc["event_count"] = cnt;
  doc["events"]      = arr;
  server.send(200, "application/json", JSON.stringify(doc));
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH);
  pinMode(LED_PIN,   OUTPUT); digitalWrite(LED_PIN,   HIGH);

  memset(events, 0, sizeof(events));
  configured = loadConfig();
  loadEvents();

  if (configured && strlen(cfgSSID) > 0) {
    Serial.print("Connecting to: "); Serial.print(cfgSSID); Serial.println(":");
    Serial.print("Using Password: "); Serial.print(cfgPass); Serial.println(":");
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSSID, cfgPass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis()-t0 < 50000) {
      delay(1500); Serial.print('.'); digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println(); Serial.print("IP: "); Serial.println(WiFi.localIP());
      settimeofday_cb(ntpSyncCb);
      // configTime() resets TZ internally, so applyTimezone() must come after.
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      applyTimezone();
      Serial.print("Waiting for NTP");
      unsigned long t1 = millis();
      while (!ntpSynced && millis()-t1 < 10000) { delay(200); Serial.print('.'); yield(); }
      Serial.println(ntpSynced ? " OK" : " timeout");
      Serial.print("TZ string: "); Serial.println(cfgTz);
      struct tm t = getLocalTime();
      int sm = calcSunset(t.tm_year+1900, t.tm_mon+1, t.tm_mday, cfgLat, cfgLon);
      if (sm >= 0) { sunsetHour=sm/60; sunsetMin=sm%60; sunsetValid=true; }
      // Start mDNS so device is reachable at <hostname>.local
      char mdnsHost[33];
      makeMdnsHostname(mdnsHost, sizeof(mdnsHost));
      if (MDNS.begin(mdnsHost)) {
        MDNS.addService("http", "tcp", 80);
        Serial.print("mDNS: http://");
        Serial.print(mdnsHost);
        Serial.println(".local");
      } else {
        Serial.println("mDNS failed");
      }
      digitalWrite(LED_PIN, HIGH);
    } else {
      Serial.println("\nWiFi failed, starting AP");
      configured = false;
    }
  }

  if (!configured) {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP("Net Timer");
    Serial.println("AP mode: Net Timer / 192.168.4.1");
    for (int i=0; i<6; i++) { digitalWrite(LED_PIN,LOW); delay(150); digitalWrite(LED_PIN,HIGH); delay(150); }
  }

  server.on("/",                 HTTP_GET,    handleRoot);
  server.on("/",                 HTTP_POST,   handleRelayPost);
  server.on("/relay",            HTTP_POST,   handleRelayPost);
  server.on("/set",              handleSet);
  server.on("/config",           handleConfig);
  server.on("/reset",            handleReset);
  server.on("/api/event",        HTTP_POST,   handleApiEvent);
  server.on("/api/event/add",    HTTP_POST,   handleApiAddEvent);
  server.on("/api/event/delete", HTTP_DELETE, handleApiDeleteEvent);
  server.on("/api/relay",        HTTP_POST,   handleApiRelay);
  server.on("/api/status",       HTTP_GET,    handleApiStatus);
  server.onNotFound([]() { server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.begin();
  Serial.println("HTTP server started");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
unsigned long lastCheck = 0;

void loop() {
  MDNS.update();
  server.handleClient();
  unsigned long now = millis();
  if (ntpSynced && now - lastCheck > 10000UL) { lastCheck = now; checkEvents(); }
}
