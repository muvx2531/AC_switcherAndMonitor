/* Remote link to the cabinet RPi: protocol in docs/remote-link-protocol.md (proto 1).
 *
 * The board is the TCP client. It scans <subnet>.ip_first..ip_last (or one fixed IP) on the
 * configured port, authenticates both ways (HMAC-SHA256 over the shared key and two random
 * nonces), then sends one JSON object per line: "status" every interval_s, "event" for new
 * event-log records, and answers to the RPi's requests (relay, get, time, ping).
 *
 * Relay commands go through Control_Submit with SRC_REMOTE, so the button control mode lock
 * (LOCAL_CONTROL) and every Control check apply. On top of that this module enforces a per
 * channel minimum gap and a per minute limit, and remembers the last 32 request ids of the
 * connection so a repeated request is answered again instead of being executed twice.
 *
 * Losing the link never changes a relay. Every socket call has a timeout; the task runs at
 * low priority and is not registered with the Supervisor. */
#include "remote_link.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include "core/net.h"
#include "core/socket.h"
#include "ipv4/ipv4.h"
#include "acquisition.h"
#include "boot.h"
#include "control.h"
#include "debug_log.h"
#include "eth_probe.h"
#include "fota.h"
#include "fw_version.h"
#include "power.h"
#include "pq_history.h"
#include "ssw_sha256.h"
#include "storage.h"
#include "web_server.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern RNG_HandleTypeDef hrng;

#define PROTO_VERSION    1
#define RX_MAX           1024U      /* longest line accepted from the RPi */
#define CONNECT_MS       500U
#define HANDSHAKE_MS     5000U
#define IDLE_CLOSE_MS    30000U     /* nothing from the RPi for this long: close and scan again */
#define SEND_MS          2000U
#define TICK_MS          50U
#define SWEEP_PAUSE_MS   10000U
#define CMD_SLOTS        32U
#define EVENTS_PER_TICK  16U
#define HIST_MAX_ROWS    120U

/* ---- buffers (CCM: CPU only, copied into the TCP buffers) ------------------- */
static char out[1536] __attribute__((section(".ccmbss")));     /* small messages, streamed */
static char rx[RX_MAX] __attribute__((section(".ccmbss")));
static char line[RX_MAX + 1U] __attribute__((section(".ccmbss")));
static StEvent evbuf[EVENTS_PER_TICK > 50U ? EVENTS_PER_TICK : 50U] __attribute__((section(".ccmbss")));
static StConfig cfg __attribute__((section(".ccmbss")));
static uint32_t stack[768] __attribute__((section(".ccmram")));
static StaticTask_t tcb;
static TaskHandle_t task;

static Socket *sock;
static bool sendErr;
static size_t outLen, rxLen;
static bool rxOverflow;

static RemoteStatus st;              /* published under a critical section */
static uint32_t lastGood;            /* last peer that passed the handshake (network order) */

typedef struct {
  char id[33];
  char actor[33];
  uint32_t req_id;
  const char *err;                   /* NULL = accepted */
  bool used, result_sent;
  const char *result_state, *result_reason;
} Cmd;
static Cmd cmds[CMD_SLOTS];
static uint32_t cmdNext;

static TickType_t chanLast[CONTROL_CHANNELS];
static bool chanUsed[CONTROL_CHANNELS];
static TickType_t minuteT[60];
static uint32_t minuteN;

/* ---- status ---------------------------------------------------------------- */
const char *RemoteLink_StateName(RemoteState s)
{
  static const char *const n[] = { "DISABLED", "NO_KEY", "NO_NETWORK", "SCANNING", "WAITING", "HANDSHAKE", "CONNECTED" };
  return (unsigned)s < sizeof(n) / sizeof(n[0]) ? n[s] : "UNKNOWN";
}

void RemoteLink_GetStatus(RemoteStatus *o) { taskENTER_CRITICAL(); *o = st; taskEXIT_CRITICAL(); }

static void setState(RemoteState s, uint32_t peer)
{
  taskENTER_CRITICAL();
  st.state = s;
  if (peer) st.peer_ip = peer;
  if (s != REMOTE_CONNECTED) st.connected_s = 0;
  taskEXIT_CRITICAL();
}

static void count(uint32_t *field) { taskENTER_CRITICAL(); (*field)++; taskEXIT_CRITICAL(); }

/* ---- HMAC-SHA256 ----------------------------------------------------------- */
static void hmacHex(const char *key, const char *label, const char *n1, const char *n2, char hex[65])
{
  uint8_t k[64] = {0}, pad[64], mac[32];
  size_t kl = strlen(key);
  if (kl > 64U) kl = 64U;            /* keys are 16..64 characters */
  memcpy(k, key, kl);
  SswSha256 c;
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36U;
  SswSha256_Init(&c);
  SswSha256_Update(&c, pad, 64);
  SswSha256_Update(&c, label, strlen(label));
  SswSha256_Update(&c, ":", 1);
  SswSha256_Update(&c, n1, strlen(n1));
  SswSha256_Update(&c, ":", 1);
  SswSha256_Update(&c, n2, strlen(n2));
  SswSha256_Final(&c, mac);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5CU;
  SswSha256_Init(&c);
  SswSha256_Update(&c, pad, 64);
  SswSha256_Update(&c, mac, 32);
  SswSha256_Final(&c, mac);
  static const char hx[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) { hex[2 * i] = hx[mac[i] >> 4]; hex[2 * i + 1] = hx[mac[i] & 15U]; }
  hex[64] = '\0';
  memset(k, 0, sizeof(k)); memset(pad, 0, sizeof(pad));
}

static bool equalCt(const char *a, const char *b, size_t n)
{
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0U;
}

static bool randomHex(char *hex, unsigned int bytes)
{
  static const char hx[] = "0123456789abcdef";
  for (unsigned int i = 0; i < bytes; i += 4U) {
    uint32_t v = 0;
    int tries = 0;
    while (HAL_RNG_GenerateRandomNumber(&hrng, &v) != HAL_OK) {   /* busy: the web uses it too */
      if (++tries > 20) return false;
      osDelay(1);
    }
    for (unsigned int j = 0; j < 4U && i + j < bytes; j++) {
      uint8_t b = (uint8_t)(v >> (8U * j));
      hex[2U * (i + j)] = hx[b >> 4]; hex[2U * (i + j) + 1U] = hx[b & 15U];
    }
  }
  hex[2U * bytes] = '\0';
  return true;
}

static bool isHex(const char *s, size_t n)
{
  if (strlen(s) != n) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

/* ---- minimal JSON reader for one flat object -------------------------------- */
static const char *ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static const char *skipValue(const char *p)
{
  p = ws(p);
  if (*p == '"') {
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p == '"' ? p + 1 : NULL;
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; if (!*p) return NULL; }
      else if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { if (--depth == 0) return p + 1; }
    }
    return NULL;
  }
  const char *s = p;
  while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
  return p == s ? NULL : p;
}

static bool validObject(const char *s)
{
  s = ws(s);
  if (*s != '{') return false;
  const char *e = skipValue(s);
  return e != NULL && *ws(e) == '\0';
}

/* Value of a top-level key, or NULL. */
static const char *jfind(const char *obj, const char *key)
{
  const char *p = ws(obj);
  if (*p++ != '{') return NULL;
  size_t kl = strlen(key);
  for (;;) {
    p = ws(p);
    if (*p != '"') return NULL;
    const char *k = ++p;
    while (*p && *p != '"') { if (*p == '\\') return NULL; p++; }
    if (*p != '"') return NULL;
    bool match = (size_t)(p - k) == kl && memcmp(k, key, kl) == 0;
    p = ws(p + 1);
    if (*p++ != ':') return NULL;
    p = ws(p);
    if (match) return p;
    p = skipValue(p);
    if (p == NULL) return NULL;
    p = ws(p);
    if (*p++ != ',') return NULL;
  }
}

static void putUtf8(char *o, size_t size, size_t *n, uint32_t cp)
{
  uint8_t b[3]; size_t len;
  if (cp < 0x80U) { b[0] = (uint8_t)cp; len = 1; }
  else if (cp < 0x800U) { b[0] = (uint8_t)(0xC0U | (cp >> 6)); b[1] = (uint8_t)(0x80U | (cp & 63U)); len = 2; }
  else { b[0] = (uint8_t)(0xE0U | (cp >> 12)); b[1] = (uint8_t)(0x80U | ((cp >> 6) & 63U)); b[2] = (uint8_t)(0x80U | (cp & 63U)); len = 3; }
  if (*n + len < size) { memcpy(o + *n, b, len); *n += len; }
}

/* String value with JSON escapes decoded (\uXXXX to UTF-8; surrogates become '?').
 * false if missing, not a string or longer than size - 1 bytes. */
static bool jstr(const char *obj, const char *key, char *o, size_t size)
{
  const char *p = jfind(obj, key);
  if (p == NULL || *p != '"') return false;
  size_t n = 0;
  for (p++; *p && *p != '"'; p++) {
    char c = *p;
    if (c == '\\') {
      c = *++p;
      if (c == 'u') {
        uint32_t cp = 0;
        for (int i = 1; i <= 4; i++) {
          char h = p[i];
          cp <<= 4;
          if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
          else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
          else return false;
        }
        p += 4;
        if (cp >= 0xD800U && cp <= 0xDFFFU) cp = '?';
        size_t before = n;
        putUtf8(o, size, &n, cp);
        if (n == before) return false;
        continue;
      }
      c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
    }
    if (n + 1U >= size) return false;
    o[n++] = c;
  }
  o[n] = '\0';
  return *p == '"';
}

/* Unsigned integer value (no sign, fraction or exponent). */
static bool jnum(const char *obj, const char *key, uint32_t *v)
{
  const char *p = jfind(obj, key);
  if (p == NULL || *p < '0' || *p > '9') return false;
  uint64_t x = 0;
  while (*p >= '0' && *p <= '9') { x = x * 10U + (uint64_t)(*p++ - '0'); if (x > 0xFFFFFFFFULL) return false; }
  if (*p == '.' || *p == 'e' || *p == 'E') return false;
  *v = (uint32_t)x;
  return true;
}

/* Array of small unsigned integers. Returns the count, -1 if missing or malformed. */
static int jarr(const char *obj, const char *key, uint32_t *v, int max)
{
  const char *p = jfind(obj, key);
  if (p == NULL || *p++ != '[') return -1;
  int n = 0;
  p = ws(p);
  if (*p == ']') return 0;
  for (;;) {
    p = ws(p);
    if (*p < '0' || *p > '9' || n >= max) return -1;
    uint32_t x = 0;
    while (*p >= '0' && *p <= '9' && x < 100000U) x = x * 10U + (uint32_t)(*p++ - '0');
    v[n++] = x;
    p = ws(p);
    if (*p == ']') return n;
    if (*p++ != ',') return -1;
  }
}

/* ---- output ---------------------------------------------------------------- */
static void rawSend(const void *d, size_t n, bool more)
{
  if (sendErr || n == 0U || sock == NULL) return;
  (void)socketSetTimeout(sock, SEND_MS);
  size_t w = 0;
  error_t e = socketSend(sock, d, n, &w, more ? SOCKET_FLAG_DELAY : SOCKET_FLAG_NO_DELAY);
  if (e != NO_ERROR || w != n) sendErr = true;
}

static void oflush(bool more) { rawSend(out, outLen, more); outLen = 0; }

static void oput(const char *fmt, ...)
{
  for (int attempt = 0; attempt < 2; attempt++) {
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(out + outLen, sizeof(out) - outLen, fmt, a);
    va_end(a);
    if (n >= 0 && (size_t)n < sizeof(out) - outLen) { outLen += (size_t)n; return; }
    if (outLen == 0U) { sendErr = true; return; }      /* one piece larger than the buffer: bug */
    oflush(true);
  }
}

/* JSON string, escaped. NULL prints null. */
static void ostr(const char *s)
{
  if (s == NULL) { oput("null"); return; }
  oput("\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') oput("\\%c", c);
    else if (c < 0x20U) oput("\\u%04x", c);
    else oput("%c", c);
  }
  oput("\"");
}

static void oend(void) { oput("\n"); oflush(false); }

static void ochannels(uint32_t mask)
{
  oput("[");
  bool first = true;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++)
    if (mask & (1U << ch)) { oput("%s%u", first ? "" : ",", ch + 1U); first = false; }
  oput("]");
}

static void oip(uint32_t ip)
{
  oput("\"%u.%u.%u.%u\"", (unsigned)(ip & 255U), (unsigned)((ip >> 8) & 255U), (unsigned)((ip >> 16) & 255U), (unsigned)(ip >> 24));
}

static void ounixNow(void)
{
  if (Boot_TimeValid()) oput("%lu", (unsigned long)Boot_UnixNow()); else oput("null");
}

static void ostdError(const char *id, const char *err)
{
  oput("{\"t\":\"error\",\"id\":"); ostr(id); oput(",\"error\":\"%s\"}", err); oend();
}

/* ---- events ---------------------------------------------------------------- */
static const char *actorOf(uint32_t req_id)
{
  for (unsigned int i = 0; i < CMD_SLOTS; i++)
    if (cmds[i].used && cmds[i].err == NULL && cmds[i].req_id == req_id) return cmds[i].actor;
  return NULL;
}

static void oevent(const StEvent *e)
{
  static const char *const causes[] = { "UNKNOWN", "LOW_POWER", "WWDG", "IWDG", "SOFTWARE", "POWER_ON", "BROWN_OUT", "PIN" };
  static const char *const actions[] = { "SET_ON", "SET_OFF", "POWER_CYCLE", "CANCEL_CYCLE" };
  static const char *const sources[] = { "WEB", "SERIAL", "BUTTON", "SYSTEM", "REMOTE" };
  oput("\"seq\":%lu,\"unix\":", (unsigned long)e->seq);
  /* rtc_s is wall time only once the clock was set (before that the RTC counts from 2000) */
  if (e->rtc_s != 0U && e->rtc_s + BOOT_UNIX_2000 >= 1704067200UL) oput("%lu", (unsigned long)(e->rtc_s + BOOT_UNIX_2000));
  else oput("null");
  oput(",\"up_s\":%lu,\"type\":", (unsigned long)e->uptime_s);
  int32_t a = e->a, b = e->b, c = e->c;
  switch ((StEventType)e->type) {
    case ST_EV_BOOT:
      oput("\"BOOT\",\"reset_cause\":\"%s\",\"reset_count\":%ld,\"safe_mode\":%s",
           (a >= 0 && a < 8) ? causes[a] : "UNKNOWN", (long)b, c ? "true" : "false");
      break;
    case ST_EV_RELAY: {
      uint32_t src = ((uint32_t)c >> 8) & 255U;
      oput("\"RELAY\",\"request_id\":%ld,\"action\":\"%s\",\"channels\":", (long)a,
           (b >= 0 && b < 4) ? actions[b] : "UNKNOWN");
      ochannels((uint32_t)c & 255U);
      oput(",\"outcome\":\"%s\",\"source\":\"%s\",\"actor\":", ((uint32_t)c >> 16) & 1U ? "APPLIED" : "FAILED",
           src < 5U ? sources[src] : "UNKNOWN");
      ostr(src == SRC_REMOTE ? actorOf((uint32_t)a) : NULL);
      break;
    }
    case ST_EV_RULE_C: oput("\"RULE_C\",\"channels\":"); ochannels((uint32_t)a); break;
    case ST_EV_SUPPLY: oput("\"SUPPLY\",\"present\":%s", a ? "true" : "false"); break;
    case ST_EV_POWER_MODE:
      oput("\"POWER_MODE\",\"from\":\"%s\",\"to\":\"%s\"", Power_ModeName((PowerMode)a), Power_ModeName((PowerMode)b));
      break;
    case ST_EV_PQ:
      oput("\"PQ\",\"kind\":\"%s\",\"duration_ms\":%ld,\"extreme_v_x10\":%ld", Acquisition_EventName((AcqPqEvent)a), (long)b, (long)c);
      break;
    case ST_EV_CONTACT:
      oput("\"CONTACT\",\"channel\":%ld,\"state\":\"%s\"", (long)a + 1L, Acquisition_ContactName((AcqPqContact)b));
      break;
    case ST_EV_CT_REVERSED: oput("\"CT_REVERSED\",\"set\":%s", a ? "true" : "false"); break;
    case ST_EV_CONFIG: oput("\"CONFIG\",\"remote\":%s", c == 1 ? "true" : "false"); break;
    case ST_EV_FORMAT: oput("\"FORMAT\""); break;
    case ST_EV_FIRMWARE:
      oput("\"FIRMWARE\",\"state\":\"%s\",\"result\":\"%s\",\"version\":\"%lu.%lu.%lu\"", Fota_StateName((SswBootState)a),
           Fota_ResultName((SswResult)b), (unsigned long)((uint32_t)c >> 16), (unsigned long)(((uint32_t)c >> 8) & 255U),
           (unsigned long)((uint32_t)c & 255U));
      break;
    case ST_EV_REMOTE_CMD: {
      char act[9];
      memcpy(act, &b, 4); memcpy(act + 4, &c, 4); act[8] = '\0';
      oput("\"REMOTE_CMD\",\"request_id\":%ld,\"actor\":", (long)a); ostr(act);
      break;
    }
    case ST_EV_REMOTE_LINK: oput("\"REMOTE_LINK\",\"connected\":%s,\"peer\":", a ? "true" : "false"); oip((uint32_t)b); break;
    default: oput("\"TYPE_%u\",\"a\":%ld,\"b\":%ld,\"c\":%ld", e->type, (long)a, (long)b, (long)c); break;
  }
}

static void sendNewEvents(uint32_t *lastEv)
{
  uint32_t last = Storage_LastEventSeq();
  if (last < *lastEv) *lastEv = last;                  /* storage formatted */
  if (last == *lastEv) return;
  uint32_t n = last - *lastEv;
  if (n > EVENTS_PER_TICK) n = EVENTS_PER_TICK;        /* older ones: RPi asks with get events */
  uint32_t got = Storage_ReadEventsBefore(last + 1U, evbuf, n);
  for (uint32_t i = got; i-- > 0U; ) {
    oput("{\"t\":\"event\","); oevent(&evbuf[i]); oput("}"); oend();
  }
  *lastEv = last;
}

/* ---- requests -------------------------------------------------------------- */
static Cmd *findCmd(const char *id)
{
  for (unsigned int i = 0; i < CMD_SLOTS; i++) if (cmds[i].used && strcmp(cmds[i].id, id) == 0) return &cmds[i];
  return NULL;
}

static void sendAck(const Cmd *c)
{
  oput("{\"t\":\"ack\",\"id\":"); ostr(c->id);
  if (c->err) oput(",\"ok\":false,\"error\":\"%s\"}", c->err);
  else oput(",\"ok\":true,\"request_id\":%lu}", (unsigned long)c->req_id);
  oend();
}

static void sendResult(const Cmd *c)
{
  oput("{\"t\":\"result\",\"id\":"); ostr(c->id);
  oput(",\"request_id\":%lu,\"state\":\"%s\",\"reason\":", (unsigned long)c->req_id, c->result_state);
  ostr(c->result_reason); oput("}"); oend();
}

static bool rateLimited(CtrlType type, uint8_t mask, const StRemoteConfig *rc)
{
  TickType_t now = xTaskGetTickCount();
  uint32_t recent = 0;
  for (uint32_t i = 0; i < minuteN && i < 60U; i++) if (now - minuteT[i] < pdMS_TO_TICKS(60000)) recent++;
  if (recent >= rc->max_per_min) return true;
  if (type == CTRL_CANCEL_CYCLE) return false;          /* only gives power back */
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++)
    if ((mask & (1U << ch)) && chanUsed[ch] && now - chanLast[ch] < pdMS_TO_TICKS((uint32_t)rc->min_gap_s * 1000U)) return true;
  return false;
}

static void noteAccepted(CtrlType type, uint8_t mask)
{
  TickType_t now = xTaskGetTickCount();
  minuteT[minuteN % 60U] = now; minuteN++;
  if (type == CTRL_CANCEL_CYCLE) return;
  for (unsigned int ch = 0; ch < CONTROL_CHANNELS; ch++) if (mask & (1U << ch)) { chanLast[ch] = now; chanUsed[ch] = true; }
}

static void handleRelay(const char *msg, const char *id)
{
  Cmd *dup = findCmd(id);
  if (dup) { sendAck(dup); if (dup->result_sent) sendResult(dup); return; }
  Cmd *c = &cmds[cmdNext++ % CMD_SLOTS];
  memset(c, 0, sizeof(*c));
  c->used = true;
  strcpy(c->id, id);

  char action[16], reason[72];
  uint32_t ch[8], dur = 0;
  int n = jarr(msg, "channels", ch, 8);
  ControlRequest req = { .source = SRC_REMOTE };
  bool ok = jstr(msg, "action", action, sizeof(action)) && n > 0 &&
            jstr(msg, "actor", c->actor, sizeof(c->actor)) && c->actor[0] != '\0';
  if (ok) {
    if (strcmp(action, "SET_ON") == 0) req.type = CTRL_SET_ON;
    else if (strcmp(action, "SET_OFF") == 0) req.type = CTRL_SET_OFF;
    else if (strcmp(action, "POWER_CYCLE") == 0) req.type = CTRL_POWER_CYCLE;
    else if (strcmp(action, "CANCEL_CYCLE") == 0) req.type = CTRL_CANCEL_CYCLE;
    else ok = false;
  }
  for (int i = 0; ok && i < n; i++) {
    if (ch[i] < 1U || ch[i] > CONTROL_CHANNELS || (req.channel_mask & (1U << (ch[i] - 1U)))) ok = false;
    else req.channel_mask |= (uint8_t)(1U << (ch[i] - 1U));
  }
  if (ok && jfind(msg, "duration_s") != NULL) {
    if (!jnum(msg, "duration_s", &dur) || dur < 1U || dur > CONTROL_MAX_CYCLE_S) ok = false;
    else req.duration_s = (uint16_t)dur;
  }
  if (ok && jfind(msg, "reason") != NULL && !jstr(msg, "reason", reason, 65)) ok = false;
  if (!ok) c->err = "INVALID";
  else if (rateLimited(req.type, req.channel_mask, &cfg.remote)) c->err = "RATE_LIMIT";
  else {
    CtrlSubmitResult r = Control_Submit(&req, &c->req_id);
    if (r == CTRL_SUBMIT_ACCEPTED) {
      noteAccepted(req.type, req.channel_mask);
      int32_t w[2] = {0, 0};
      memcpy(w, c->actor, strnlen(c->actor, 8));
      Storage_LogEvent(ST_EV_REMOTE_CMD, (int32_t)c->req_id, w[0], w[1], 0);
    } else if (r == CTRL_SUBMIT_NOT_READY) c->err = "BUSY";
    else c->err = Control_SubmitResultName(r);
  }
  count(c->err ? &st.rejected : &st.commands);
  (void)DebugLog_Printf("REMOTE: relay %s mask %X -> %s%s (request %lu)\r\n", ok ? action : "?", req.channel_mask,
      c->err ? "rejected " : "accepted", c->err ? c->err : "", (unsigned long)c->req_id);
  sendAck(c);
}

static void pollResults(void)
{
  for (unsigned int i = 0; i < CMD_SLOTS; i++) {
    Cmd *c = &cmds[i];
    if (!c->used || c->err || c->result_sent) continue;
    CtrlOutcome o;
    if (!Control_GetOutcome(c->req_id, &o)) { c->result_state = "UNKNOWN"; c->result_reason = NULL; }
    else if (o.state == OUT_PENDING) continue;
    else { c->result_state = Control_OutcomeName(o.state); c->result_reason = o.reason; }
    c->result_sent = true;
    sendResult(c);
  }
}

/* Sink for WebServer_WithJson: runs with the web JSON mutex held; the header is already in
 * out[] and is sent together with the document (SEND_MS bounds the wait). */
static void jsonSink(const char *json, size_t len, void *ctx)
{
  (void)ctx;
  oflush(true);
  rawSend(json, len, true);
  oput("}"); oend();
}

static void sendJsonDoc(const char *id, const char *what, WebJsonKind kind)
{
  oput("{\"t\":\"data\",\"id\":"); ostr(id); oput(",\"what\":\"%s\",\"data\":", what);
  if (!WebServer_WithJson(kind, jsonSink, NULL)) { outLen = 0; ostdError(id, "BUSY"); }
}

static void sendEvents(const char *msg, const char *id)
{
  uint32_t n = 20, before = UINT32_MAX;
  if (jfind(msg, "n") && (!jnum(msg, "n", &n) || n < 1U || n > 50U)) { ostdError(id, "INVALID"); return; }
  if (jfind(msg, "before_seq") && !jnum(msg, "before_seq", &before)) { ostdError(id, "INVALID"); return; }
  uint32_t got = Storage_ReadEventsBefore(before, evbuf, n);
  oput("{\"t\":\"data\",\"id\":"); ostr(id); oput(",\"what\":\"events\",\"data\":{\"last_seq\":%lu,\"events\":[",
      (unsigned long)Storage_LastEventSeq());
  for (uint32_t i = 0; i < got; i++) { oput("%s{", i ? "," : ""); oevent(&evbuf[i]); oput("}"); }
  oput("]}}"); oend();
}

static void ohist(int16_t v) { if (v == HIST_MISSING) oput(",null"); else oput(",%d", v); }

static void sendHistory(const char *msg, const char *id)
{
  uint32_t total = History_Count(), off = 0, n = 60;
  if (jfind(msg, "offset") && !jnum(msg, "offset", &off)) { ostdError(id, "INVALID"); return; }
  if (jfind(msg, "n") && (!jnum(msg, "n", &n) || n < 1U || n > HIST_MAX_ROWS)) { ostdError(id, "INVALID"); return; }
  if (off > total) off = total;
  if (n > total - off) n = total - off;
  uint32_t upNow = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ), endS = History_NewestEndS();
  oput("{\"t\":\"data\",\"id\":"); ostr(id);
  oput(",\"what\":\"history\",\"data\":{\"period_s\":%u,\"total\":%lu,\"offset\":%lu,\"up_now_s\":%lu,\"unix_now\":",
      HIST_PERIOD_S, (unsigned long)total, (unsigned long)off, (unsigned long)upNow);
  ounixNow();
  oput(",\"fields\":[\"end_up_s\",\"v_avg_x10\",\"v_min_x10\",\"v_max_x10\",\"i_ma\",\"p_dw\",\"freq_mhz\",\"pf_x1000\",\"v_thd_x10\",\"i_thd_x10\"],\"rows\":[");
  for (uint32_t i = 0; i < n; i++) {
    HistEntry e;
    uint32_t idx = off + i;
    if (!History_Get(idx, &e)) break;
    oput("%s[%lu", i ? "," : "", (unsigned long)(endS - (total - 1U - idx) * HIST_PERIOD_S));
    ohist(e.v_avg_dv); ohist(e.v_min_dv); ohist(e.v_max_dv); ohist(e.i_ma); ohist(e.p_dw);
    if (e.f_off_mhz == HIST_MISSING) oput(",null"); else oput(",%ld", 50000L + e.f_off_mhz);
    ohist(e.pf_x1000); ohist(e.v_thd_x10); ohist(e.i_thd_x10);
    oput("]");
  }
  oput("]}}"); oend();
}

static void handleGet(const char *msg, const char *id)
{
  char what[16];
  if (!jstr(msg, "what", what, sizeof(what))) { ostdError(id, "INVALID"); return; }
  if (strcmp(what, "status") == 0) sendJsonDoc(id, what, WEB_JSON_STATUS);
  else if (strcmp(what, "waveform") == 0) sendJsonDoc(id, what, WEB_JSON_WAVEFORM);
  else if (strcmp(what, "fw") == 0) sendJsonDoc(id, what, WEB_JSON_FW);
  else if (strcmp(what, "events") == 0) sendEvents(msg, id);
  else if (strcmp(what, "history") == 0) sendHistory(msg, id);
  else ostdError(id, "INVALID");
}

static void handleLine(const char *msg)
{
  char t[16], id[33];
  if (!validObject(msg)) { ostdError(NULL, "BAD_JSON"); return; }
  if (!jstr(msg, "t", t, sizeof(t))) { ostdError(NULL, "UNKNOWN_TYPE"); return; }
  bool idOk = jstr(msg, "id", id, sizeof(id)) && id[0] != '\0';
  for (const char *p = id; idOk && *p; p++) if ((unsigned char)*p < 0x20U || *p == '"' || *p == '\\') idOk = false;
  if (!idOk) { ostdError(NULL, "MISSING_ID"); return; }
  if (strcmp(t, "relay") == 0) handleRelay(msg, id);
  else if (strcmp(t, "get") == 0) handleGet(msg, id);
  else if (strcmp(t, "ping") == 0) { oput("{\"t\":\"pong\",\"id\":"); ostr(id); oput("}"); oend(); }
  else if (strcmp(t, "time") == 0) {
    uint32_t u = 0;
    bool ok = jnum(msg, "unix", &u) && Boot_SetUnixTime(u);
    if (ok) (void)DebugLog_Printf("REMOTE: clock set by the RPi\r\n");
    oput("{\"t\":\"ack\",\"id\":"); ostr(id);
    if (ok) oput(",\"ok\":true}"); else oput(",\"ok\":false,\"error\":\"INVALID\"}");
    oend();
  }
  else ostdError(id, "UNKNOWN_TYPE");
}

/* ---- receive --------------------------------------------------------------- */
/* Waits up to TICK_MS for data. false: connection closed or failed. */
static bool pollRx(void)
{
  if (rxLen >= RX_MAX) { rxOverflow = true; return true; }
  (void)socketSetTimeout(sock, TICK_MS);
  size_t got = 0;
  error_t e = socketReceive(sock, rx + rxLen, RX_MAX - rxLen, &got, 0);
  if (e == NO_ERROR || got > 0U) { rxLen += got; return true; }
  return e == ERROR_TIMEOUT || e == ERROR_WOULD_BLOCK;
}

/* Next complete line into line[]; false if none yet. */
static bool nextLine(void)
{
  char *nl = memchr(rx, '\n', rxLen);
  if (nl == NULL) { if (rxLen >= RX_MAX) rxOverflow = true; return false; }
  size_t n = (size_t)(nl - rx);
  memcpy(line, rx, n);
  line[n] = '\0';
  if (n && line[n - 1U] == '\r') line[n - 1U] = '\0';
  rxLen -= n + 1U;
  memmove(rx, nl + 1, rxLen);
  return true;
}

/* ---- connection ------------------------------------------------------------ */
static void closeSock(void)
{
  if (sock == NULL) return;
  (void)socketSetTimeout(sock, 300);
  (void)socketShutdown(sock, SOCKET_SD_BOTH);
  socketClose(sock);
  sock = NULL;
}

static bool configChanged(uint32_t seq)
{
  static StConfig now __attribute__((section(".ccmbss")));
  Storage_GetConfig(&now);
  return now.seq != seq;
}

static bool networkUp(void)
{
  EthInfo n;
  EthProbe_GetInfo(&n);
  return n.available && n.powered && n.link && n.addressValid;
}

static bool handshake(uint32_t peer)
{
  char nb[33], nr[40], proof[72], mine[65];
  EthInfo n;
  EthProbe_GetInfo(&n);
  if (!randomHex(nb, 16)) return false;
  setState(REMOTE_HANDSHAKE, peer);
  oput("{\"t\":\"hello\",\"proto\":%d,\"device\":\"smartswitch\",\"mac\":\"%s\",\"fw\":\"%s\",\"nonce\":\"%s\"}",
       PROTO_VERSION, n.mac, FW_VERSION_STR, nb);
  oend();
  TickType_t start = xTaskGetTickCount();
  while (!sendErr && xTaskGetTickCount() - start < pdMS_TO_TICKS(HANDSHAKE_MS)) {
    if (!pollRx() || rxOverflow) return false;
    if (!nextLine()) continue;
    char t[12];
    if (!validObject(line) || !jstr(line, "t", t, sizeof(t)) || strcmp(t, "auth") != 0 ||
        !jstr(line, "nonce", nr, sizeof(nr)) || !isHex(nr, 32) || !jstr(line, "proof", proof, sizeof(proof)) || !isHex(proof, 64))
      return false;
    for (char *p = proof; *p; p++) if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
    hmacHex(cfg.remote.key, "SSW1-RPI", nb, nr, mine);
    if (!equalCt(mine, proof, 64)) {
      oput("{\"t\":\"auth_fail\"}"); oend();
      count(&st.auth_failures);
      (void)DebugLog_Printf("REMOTE: %u.%u.%u.%u failed the handshake (wrong key?)\r\n", (unsigned)(peer & 255U),
          (unsigned)((peer >> 8) & 255U), (unsigned)((peer >> 16) & 255U), (unsigned)(peer >> 24));
      return false;
    }
    hmacHex(cfg.remote.key, "SSW1-BOARD", nr, nb, mine);
    oput("{\"t\":\"auth_ok\",\"proof\":\"%s\",\"status_interval_s\":%u}", mine, cfg.remote.interval_s);
    oend();
    return !sendErr;
  }
  return false;
}

static void session(uint32_t peer)
{
  memset(cmds, 0, sizeof(cmds));
  cmdNext = 0;
  lastGood = peer;
  count(&st.connects);
  setState(REMOTE_CONNECTED, peer);
  Storage_LogEvent(ST_EV_REMOTE_LINK, 1, (int32_t)peer, 0, 0);
  (void)DebugLog_Printf("REMOTE: connected to %u.%u.%u.%u:%u\r\n", (unsigned)(peer & 255U), (unsigned)((peer >> 8) & 255U),
      (unsigned)((peer >> 16) & 255U), (unsigned)(peer >> 24), cfg.remote.port);
  uint32_t statusSeq = 0, lastEv = Storage_LastEventSeq();
  TickType_t begin = xTaskGetTickCount(), lastRx = begin, nextStatus = begin, lastNet = begin;
  const char *why = "send failed";
  for (;;) {
    if (!pollRx()) { why = "closed by the RPi"; break; }
    while (!sendErr && nextLine()) { lastRx = xTaskGetTickCount(); handleLine(line); }
    if (rxOverflow) { ostdError(NULL, "TOO_LONG"); why = "line too long"; break; }
    if (sendErr) break;
    TickType_t now = xTaskGetTickCount();
    if (now - lastRx > pdMS_TO_TICKS(IDLE_CLOSE_MS)) { why = "RPi silent for 30 s"; break; }
    if ((int32_t)(now - nextStatus) >= 0) {
      nextStatus = now + pdMS_TO_TICKS((uint32_t)cfg.remote.interval_s * 1000U);
      oput("{\"t\":\"status\",\"seq\":%lu,\"unix\":", (unsigned long)(statusSeq + 1U));
      ounixNow();
      oput(",\"data\":");
      if (WebServer_WithJson(WEB_JSON_STATUS, jsonSink, NULL)) statusSeq++;
      else outLen = 0;                 /* busy: skip this period */
    }
    sendNewEvents(&lastEv);
    pollResults();
    if (sendErr) break;
    if (now - lastNet > pdMS_TO_TICKS(500)) {
      lastNet = now;
      if (configChanged(cfg.seq)) { why = "settings changed"; break; }
      if (!networkUp()) { why = "network down"; break; }
      taskENTER_CRITICAL(); st.connected_s = (uint32_t)((now - begin) / configTICK_RATE_HZ); taskEXIT_CRITICAL();
    }
  }
  Storage_LogEvent(ST_EV_REMOTE_LINK, 0, (int32_t)peer, 0, 0);
  (void)DebugLog_Printf("REMOTE: disconnected (%s)\r\n", why);
}

/* One attempt: connect, handshake, run the session. true if the handshake passed. */
static bool attempt(uint32_t ip)
{
  sock = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
  if (sock == NULL) return false;
  sendErr = false; outLen = 0; rxLen = 0; rxOverflow = false;
  (void)socketSetTimeout(sock, CONNECT_MS);
  IpAddr a;
  a.length = sizeof(Ipv4Addr);
  a.ipv4Addr = ip;
  bool ok = false;
  if (socketConnect(sock, &a, cfg.remote.port) == NO_ERROR && handshake(ip)) {
    ok = true;
    session(ip);
  }
  closeSock();
  return ok;
}

static void sleepUnlessChanged(uint32_t ms, uint32_t seq)
{
  for (uint32_t t = 0; t < ms; t += 500U) {
    if (configChanged(seq)) return;
    osDelay(500);
  }
}

static void remoteTask(void *arg)
{
  (void)arg;
  osDelay(3000);                         /* let DHCP settle */
  for (;;) {
    Storage_GetConfig(&cfg);
    const StRemoteConfig *rc = &cfg.remote;
    if (!rc->enabled) { setState(REMOTE_DISABLED, 0); sleepUnlessChanged(2000, cfg.seq); continue; }
    if (rc->key[0] == '\0') { setState(REMOTE_NO_KEY, 0); sleepUnlessChanged(2000, cfg.seq); continue; }
    if (!networkUp()) { setState(REMOTE_NO_NETWORK, 0); osDelay(1000); continue; }
    Ipv4Addr own = IPV4_UNSPECIFIED_ADDR;
    (void)ipv4GetHostAddr(&netInterface[0], &own);

    /* candidate list: last good peer first, then the fixed IP or the range */
    static uint32_t list[256];
    unsigned int n = 0;
    if (rc->fixed_ip != 0U) list[n++] = rc->fixed_ip;
    else {
      uint32_t base = own & 0x00FFFFFFU;  /* network order: the last octet is the top byte */
      if (lastGood != 0U && (lastGood & 0x00FFFFFFU) == base) list[n++] = lastGood;
      for (unsigned int o = rc->ip_first; o <= rc->ip_last && n < 256U; o++) {
        uint32_t ip = base | ((uint32_t)o << 24);
        if (ip != own && ip != lastGood) list[n++] = ip;
      }
    }
    setState(REMOTE_SCANNING, 0);
    bool connected = false;
    for (unsigned int i = 0; i < n; i++) {
      if (configChanged(cfg.seq) || !networkUp()) break;
      if (attempt(list[i])) { connected = true; break; }
      setState(REMOTE_SCANNING, 0);
    }
    if (!connected) {
      count(&st.sweeps);
      setState(REMOTE_WAITING, 0);
      sleepUnlessChanged(SWEEP_PAUSE_MS, cfg.seq);
    }
  }
}

bool RemoteLink_Start(void)
{
  if (task != NULL) return true;
  /* Low priority: below the Supervisor, like Storage and the local UI. */
  task = xTaskCreateStatic(remoteTask, "remote", sizeof(stack) / sizeof(stack[0]), NULL, osPriorityLow, (StackType_t *)stack, &tcb);
  if (task != NULL) (void)DebugLog_Printf("REMOTE: link task started (docs/remote-link-protocol.md)\r\n");
  return task != NULL;
}
