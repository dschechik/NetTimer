# NetTimer — ESP8266 Network Electrical Timer

A WiFi-connected relay timer for the ESP8266 ESP-01S with a web interface, JSON API, NTP time sync, sunset-based scheduling, DST-aware timezones, and mDNS discovery.

---

## Hardware

| Component | Notes |
|-----------|-------|
| [HiLetgo ESP8266 ESP-01S Relay Module](https://www.amazon.com/HiLetgo-ESP8266-Module-Control-Automation/dp/B071LMSLRW) | Relay + ESP-01S on one board |
| 5 V USB power supply | 500 mA or more |
| USB–to–TTL adapter | For initial flashing only |

### Pin Usage

| GPIO | Function |
|------|----------|
| GPIO0 | Relay control — active LOW (relay energised when LOW) |
| GPIO2 | Status LED — active LOW (built-in) |

Connect your load (lamp, pump, etc.) to the relay's **COM** and **NO** (normally-open) terminals.

---

## Arduino IDE Setup

### Board

1. Add the ESP8266 core URL in **File → Preferences → Additional Boards Manager URLs**:
   ```
   http://arduino.esp8266.com/stable/package_esp8266com_index.json
   ```
2. Install **esp8266** via **Tools → Board → Boards Manager**
3. Select **Board: Generic ESP8266 Module**
4. Recommended settings:
   - Flash Size: **1M (128K SPIFFS)** or 1M (no SPIFFS)
   - Upload Speed: 115200
   - CPU Frequency: 80 MHz

### Required Libraries

Only one library needs to be installed via the Library Manager:

| Library | Author | Install via |
|---------|--------|-------------|
| **Arduino_JSON** | Arduino | Library Manager |

Everything else is bundled with the ESP8266 Arduino core and requires no separate installation:

| Library | Purpose |
|---------|---------|
| `ESP8266WiFi` | WiFi station and AP mode |
| `ESP8266WebServer` | HTTP server |
| `ESP8266mDNS` | mDNS / `.local` hostname |
| `Preferences` | Wear-levelled flash key-value storage |
| `coredecls.h` | `settimeofday_cb()` for NTP sync callback |
| `configTime()` / `time()` | Built-in SNTP stack — no NTPClient needed |

---

## First-Time Setup

1. Flash the sketch.
2. On first boot (or after a factory reset) the device starts in **AP mode**.
3. Connect your phone or laptop to the WiFi network **"Net Timer"**.
4. Open `http://192.168.4.1` in a browser.
5. Go to **Config** and fill in:
   - Device Name (optional — used in page titles and mDNS hostname)
   - WiFi SSID and password
   - Latitude and longitude (for sunset calculation)
   - Timezone (select from dropdown)
6. Click **Save & Reboot**. The device joins your network.
7. The serial console prints the assigned IP address and mDNS hostname:
   ```
   IP: 192.168.1.45
   mDNS: http://kitchen-timer.local
   ```
8. Either address works in a browser.

---

## Web Interface

| URL | Description |
|-----|-------------|
| `/` | Status — current time, relay state, sunset, manual relay toggle |
| `/set` | Events — view, add, and delete timer events |
| `/config` | Device name, WiFi credentials, location, timezone |
| `/reset` | Factory reset — erases all settings and events |

### Status Page

Shows the current local time and date, relay state (ON/OFF), today's calculated sunset time, configured location, and NTP sync status. Two buttons allow immediate manual relay control independent of the event schedule.

### Events Page

Lists all active events with their action, time, and schedule. Each event has a Delete button. The Add Event form at the bottom covers all combinations:

- **Action** — Turn ON or Turn OFF
- **Schedule** — Repeat (selected days of week) or One-time (specific date)
- **Time type** — Specific time (HH:MM) or Sunset offset (±minutes)

### Config Page

| Field | Description |
|-------|-------------|
| Device Name | Up to 32 characters. Appears in page `<title>` and `<h1>`, used as the mDNS hostname (spaces become hyphens, special characters dropped). Optional — leave blank for plain "NetTimer". |
| WiFi SSID | Network name to join |
| WiFi Password | Stored as raw bytes to preserve all special characters |
| Latitude / Longitude | Decimal degrees. Used for sunset calculation. Positive = N/E, negative = S/W. |
| Timezone | Dropdown of 22 common zones with full DST rules (see below) |

Saving the config always triggers a reboot.

---

## Timezone Reference

Timezones are stored as POSIX TZ strings, which encode both the UTC offset and DST transition rules. The SDK applies them natively — clock changes happen automatically.

| Label | POSIX String | DST |
|-------|-------------|-----|
| Hawaii (UTC-10) | `HST10` | No |
| Alaska (UTC-9/-8) | `AKST9AKDT,M3.2.0,M11.1.0` | Yes |
| US Pacific (UTC-8/-7) | `PST8PDT,M3.2.0,M11.1.0` | Yes |
| US Mountain (UTC-7/-6) | `MST7MDT,M3.2.0,M11.1.0` | Yes |
| US Arizona (UTC-7) | `MST7` | No |
| US Central (UTC-6/-5) | `CST6CDT,M3.2.0,M11.1.0` | Yes |
| US Eastern (UTC-5/-4) | `EST5EDT,M3.2.0,M11.1.0` | Yes |
| Atlantic / Halifax (UTC-4/-3) | `AST4ADT,M3.2.0,M11.1.0` | Yes |
| Brazil / Sao Paulo (UTC-3) | `BRT3` | No |
| UTC / GMT (UTC+0) | `GMT0` | No |
| UK / Ireland (UTC+0/+1) | `GMT0BST,M3.5.0/1,M10.5.0` | Yes |
| Central Europe (UTC+1/+2) | `CET-1CEST,M3.5.0,M10.5.0/3` | Yes |
| Eastern Europe (UTC+2/+3) | `EET-2EEST,M3.5.0/3,M10.5.0/4` | Yes |
| Israel (UTC+2/+3) | `IST-2IDT,M3.4.4/26,M10.5.0` | Yes |
| Moscow (UTC+3) | `MSK-3` | No |
| Gulf / Dubai (UTC+4) | `GST-4` | No |
| India (UTC+5:30) | `IST-5:30` | No |
| W. Indonesia / Bangkok (UTC+7) | `WIB-7` | No |
| China / Singapore / HK (UTC+8) | `CST-8` | No |
| Japan / Korea (UTC+9) | `JST-9` | No |
| Australia Eastern (UTC+10/+11) | `AEST-10AEDT,M10.1.0,M4.1.0/3` | Yes |
| New Zealand (UTC+12/+13) | `NZST-12NZDT,M9.5.0,M4.1.0/3` | Yes |

---

## mDNS / `.local` Hostname

When connected to WiFi, the device advertises itself via mDNS so it can be reached by name instead of IP address.

The hostname is derived from the **Device Name** field:
- Converted to lowercase
- Spaces become hyphens
- All other non-alphanumeric characters are dropped
- Falls back to `net-timer` if the result is empty

Examples:

| Device Name | mDNS Hostname |
|-------------|--------------|
| Kitchen Timer | `http://kitchen-timer.local` |
| Garage | `http://garage.local` |
| *(empty)* | `http://net-timer.local` |

**Platform support:**
- macOS, iOS — works natively
- Windows 10+ — works with Bonjour installed (included with iTunes)
- Linux — works on most desktop distributions
- Android — support varies by version; use the IP address as a fallback

mDNS only works in station mode. In AP mode the device is always at `192.168.4.1`.

---

## Event Configuration

### Parameters

| Parameter | Options |
|-----------|---------|
| Action | Turn ON / Turn OFF |
| Schedule | Repeat (days of week) / One-time (specific date) |
| Time type | Specific time (HH:MM) / Sunset offset (±minutes from sunset) |
| Days | Any combination of Sun–Sat (repeat mode only) |

### Examples

| Goal | Settings |
|------|---------|
| Turn ON every Mon & Thu at 07:00 | Action=ON, Repeat, Days=[Mon,Thu], Time=07:00 |
| Turn OFF 30 min after sunset, daily | Action=OFF, Repeat, Days=[all], Sunset offset=+30 |
| Turn ON 15 min before sunset, weekdays | Action=ON, Repeat, Days=[Mon–Fri], Sunset offset=−15 |
| Turn ON once on 25 Dec at 18:00 | Action=ON, One-time, Date=2025-12-25, Time=18:00 |

One-shot events are automatically disabled after they fire and will not repeat.

---

## JSON API

All endpoints accept and return `application/json`. Slot numbers are 0–9.

---

### `POST /api/event/add` — Add a timer event

**Repeat event (specific time):**
```json
{
  "action":    "on",
  "time_type": "abs",
  "hour":      7,
  "minute":    30,
  "schedule":  "repeat",
  "days":      [1, 3, 5]
}
```
`days` is an array of integers: 0=Sun, 1=Mon, 2=Tue, 3=Wed, 4=Thu, 5=Fri, 6=Sat. Omit or send `[]` to repeat every day.

**Repeat event (sunset-relative):**
```json
{
  "action":     "off",
  "time_type":  "sunset",
  "sun_offset": 30,
  "schedule":   "repeat",
  "days":       [0, 1, 2, 3, 4, 5, 6]
}
```
`sun_offset` is minutes from sunset — negative fires before sunset.

**One-time event:**
```json
{
  "action":    "on",
  "time_type": "abs",
  "hour":      18,
  "minute":    0,
  "schedule":  "once",
  "year":      2025,
  "month":     12,
  "day":       25
}
```

**Response:**
```json
{ "ok": true, "slot": 3 }
```

**Errors:**
```json
{ "error": "No free slots" }
{ "error": "Missing year/month/day for once schedule" }
```

---

### `DELETE /api/event/delete` — Delete a timer event

```json
{ "slot": 3 }
```

**Response:**
```json
{ "ok": true, "slot": 3 }
```

**Errors:**
```json
{ "error": "slot must be 0-9" }
{ "error": "Slot already empty" }
```

---

### `POST /api/relay` — Set relay state immediately

```json
{ "state": "on" }
```
or
```json
{ "state": "off" }
```

**Response:**
```json
{ "ok": true, "relay": "on" }
```

---

### `GET /api/status` — Device status

```json
{
  "name":        "Kitchen Timer",
  "relay":       false,
  "ntp_synced":  true,
  "epoch":       1749999999,
  "local_time":  "2025-06-15T20:28:05",
  "sunset":      "19:38",
  "event_count": 2,
  "events": [
    { "slot": 0, "action": "on",  "use_sunset": false, "repeat": true,  "offset_min": 420 },
    { "slot": 1, "action": "off", "use_sunset": true,  "repeat": true,  "offset_min": 30  }
  ]
}
```

`name` is the configured device name, or `"NetTimer"` if none is set. `sunset` is omitted if it has not been calculated yet (appears after first midnight or on first boot after NTP sync).

---

### `POST /api/event` — Legacy one-time event (backwards compatible)

Kept for compatibility with earlier integrations. Prefer `/api/event/add` for new work.

```json
{
  "action": "on",
  "year":   2025,
  "month":  6,
  "day":    15,
  "hour":   20,
  "minute": 30
}
```

---

### Example curl commands

```bash
# Add a repeat event: ON at 07:30 Mon/Wed/Fri
curl -X POST http://kitchen-timer.local/api/event/add \
     -H 'Content-Type: application/json' \
     -d '{"action":"on","time_type":"abs","hour":7,"minute":30,"schedule":"repeat","days":[1,3,5]}'

# Add a sunset event: OFF 30 min after sunset, daily
curl -X POST http://kitchen-timer.local/api/event/add \
     -H 'Content-Type: application/json' \
     -d '{"action":"off","time_type":"sunset","sun_offset":30,"schedule":"repeat"}'

# Delete slot 2
curl -X DELETE http://kitchen-timer.local/api/event/delete \
     -H 'Content-Type: application/json' \
     -d '{"slot":2}'

# Turn relay ON immediately
curl -X POST http://kitchen-timer.local/api/relay \
     -H 'Content-Type: application/json' \
     -d '{"state":"on"}'

# Check status
curl http://kitchen-timer.local/api/status
```

---

## Storage Layout (Preferences)

Uses the ESP8266 `Preferences` library — key-value store, wear-levelled flash. No EEPROM address arithmetic required. All string fields are stored as raw bytes (`putBytes`/`getBytes`) to preserve special characters reliably, sized to `strlen + 1` (not the full buffer) to prevent off-by-one null-termination errors.

| Namespace | Key | Type | Content |
|-----------|-----|------|---------|
| `cfg` | `ssid` | bytes | WiFi SSID (up to 32 chars + null) |
| `cfg` | `pass` | bytes | WiFi password (up to 64 chars + null) |
| `cfg` | `lat` | float | Latitude |
| `cfg` | `lon` | float | Longitude |
| `cfg` | `tz` | bytes | POSIX TZ string (up to 47 chars + null) |
| `cfg` | `name` | bytes | Device name (up to 32 chars + null) |
| `ev0` … `ev9` | `d` | bytes | Raw `TimerEvent` struct (one namespace per slot) |

Factory reset clears all namespaces via `prefs.clear()`.

---

## Serial Output Reference

On boot the serial port (115200 baud) prints:

```
Connecting to: MyNetwork:
Using Password: ••••••••:
.....
IP: 192.168.1.45
Waiting for NTP..... OK
TZ string: EST5EDT,M3.2.0,M11.1.0
mDNS: http://kitchen-timer.local
HTTP server started
```

If WiFi fails after the 50-second timeout the device falls back to AP mode and prints:

```
WiFi failed, starting AP
AP mode: Net Timer / 192.168.4.1
```

---

## Operational Notes

- Events are checked every 10 seconds. Any event whose scheduled minute matches the current local minute will fire within 10 seconds of the scheduled time.
- One-shot events are automatically disabled after firing and will not trigger again.
- NTP syncs at startup (waits up to 10 seconds) and then refreshes automatically in the background every hour via the SDK's built-in SNTP stack.
- Sunset is calculated at startup (after NTP sync) and recalculated daily at midnight using the NOAA simplified algorithm. Accuracy is typically within 5–10 minutes.
- DST transitions are handled automatically by the POSIX TZ string — no manual intervention needed when clocks change.
- The relay calculates what the state should be on saved events and sets it accordingly on every power-on regardless of its previous state.
- If WiFi drops after initial connection the web interface and API become unavailable, but the relay and event schedule continue operating normally as long as time was synced at least once.
- mDNS is only active in station mode. In AP mode use `192.168.4.1`.
