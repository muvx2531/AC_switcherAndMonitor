#include "web_server.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "http/http_server.h"
#include "debug_log.h"
#include "eth_probe.h"
#include "pca9536.h"
#include "acquisition.h"
#include "power.h"
#include "pq_history.h"
#include "storage.h"
#include "fota.h"
#include "fw_version.h"
#include "boot.h"
#include "w25q128.h"
#include "boot.h"
#include "supervisor.h"
#include "control.h"
#include "web_auth.h"
#include "remote_link.h"
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define JSON_SIZE 6144U
#define MAX_TASKS 16U

static HttpServerSettings settings;
static HttpServerContext context;
static HttpConnection connections[HTTP_SERVER_MAX_CONNECTIONS];

/* One JSON builder shared by both HTTP connection tasks, guarded by a mutex. */
static SemaphoreHandle_t jsonMutex;
static StaticSemaphore_t jsonMutexStorage;
static char json[JSON_SIZE] __attribute__((section(".ccmbss")));   /* CPU only, copied into TCP buffers */
static size_t jsonLen;
static bool jsonOverflow;
static TaskStatus_t taskStatus[MAX_TASKS];

/* Flash IDs are read once at start; the page does not need live SPI traffic. */
static bool flashOk[2];
static uint8_t flashId[2][3];

static const char build[] = __DATE__ " " __TIME__;

static void put(const char *fmt, ...)
{
  if (jsonOverflow) return;
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(json + jsonLen, JSON_SIZE - jsonLen, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= JSON_SIZE - jsonLen) { jsonOverflow = true; return; }
  jsonLen += (size_t)n;
}

static unsigned pin(GPIO_TypeDef *port, uint16_t p) { return HAL_GPIO_ReadPin(port, p) == GPIO_PIN_SET ? 1U : 0U; }

static void buildStatus(void)
{
  jsonLen = 0; jsonOverflow = false;
  uint32_t uptimeMs = (uint32_t)(((uint64_t)xTaskGetTickCount() * 1000U) / configTICK_RATE_HZ);
  put("{\"fw\":{\"version\":\"" FW_VERSION_STR "\",\"build\":\"%s\",\"uptime_ms\":%lu},", build, (unsigned long)uptimeMs);

  EthInfo n;
  EthProbe_GetInfo(&n);
  put("\"net\":{\"powered\":%s,\"link\":%s,\"mode\":\"%s\",\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"mac\":\"%s\",\"phy_id\":\"%04X:%04X\"},",
      n.powered ? "true" : "false", n.link ? "true" : "false", n.dhcp ? "dhcp" : (n.addressValid ? "static" : "none"),
      n.ip, n.mask, n.gw, n.mac, n.phyId1, n.phyId2);

  /* Read-only: no relay state is changed by the web in this phase. */
  PCA9536_Registers r;
  I2cBusResult rr = PCA9536_ReadRegisters(&r);
  if (rr == I2C_BUS_OK)
    put("\"relay\":{\"ok\":true,\"err\":0,\"output\":%u,\"input\":%u,\"config\":%u},", r.output, r.input, r.config);
  else
    put("\"relay\":{\"ok\":false,\"err\":%u,\"output\":null,\"input\":null,\"config\":null},", (unsigned)rr);

  ControlSnapshot c;
  Control_GetSnapshot(&c);
  static const char *const eff[] = {"ON", "OFF", "UNKNOWN"};
  put("\"control\":{\"available\":%s,\"state_known\":%s,\"coil_supply\":%s,\"cycle_remaining_s\":[%u,%u,%u,%u],"
      "\"effective\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"glitches\":%lu,\"supply_releases\":%lu,\"boot_note\":\"%s\",\"local_lock\":%s},",
      c.available ? "true" : "false", c.state_known ? "true" : "false", c.coil_supply ? "true" : "false",
      c.cycle_remaining_s[0], c.cycle_remaining_s[1], c.cycle_remaining_s[2], c.cycle_remaining_s[3],
      eff[c.effective[0]], eff[c.effective[1]], eff[c.effective[2]], eff[c.effective[3]],
      (unsigned long)c.glitch_count, (unsigned long)c.supply_releases, c.boot_note ? c.boot_note : "",
      c.local_lock ? "true" : "false");

  PowerSnapshot ps;
  Power_GetSnapshot(&ps);
  put("\"power\":{\"vin_detect\":%u,\"efuse_ilim\":%u,\"chrg\":%u,\"fault\":%u,"
      "\"mode\":\"%s\",\"mode_age_s\":%lu,\"eth_rail\":%s,\"amc_rail\":%s,\"transitions\":%lu,"
      "\"eth_resume_fail\":%lu,\"reason\":\"%s\"},",
      pin(Vin_Detect_GPIO_Port, Vin_Detect_Pin), pin(eFuse_iLim_GPIO_Port, eFuse_iLim_Pin),
      pin(Batt_status_GPIO_Port, Batt_status_Pin), pin(Batt_FAULT_GPIO_Port, Batt_FAULT_Pin),
      Power_ModeName(ps.mode), (unsigned long)(ps.mode_age_ms / 1000U), ps.eth_rail ? "true" : "false",
      ps.amc_rail ? "true" : "false", (unsigned long)ps.transitions, (unsigned long)ps.eth_resume_failures,
      ps.last_reason ? ps.last_reason : "");

  /* Acquisition snapshot: a value is printed only when its validity is VALID, else null. */
  AcqSnapshot a;
  Acquisition_GetSnapshot(&a);
  bool vbOk = a.vbat.validity == ACQ_VALID;
  if (vbOk)
    put("\"vbat\":{\"ok\":true,\"state\":\"VALID\",\"raw\":%u,\"mv_est\":%ld,\"age_ms\":%lu},",
        a.vbat.raw, (long)a.vbat.value, (unsigned long)a.vbat.age_ms);
  else
    put("\"vbat\":{\"ok\":false,\"state\":\"%s\",\"raw\":null,\"mv_est\":null,\"age_ms\":%lu},",
        Acquisition_ValidityName(a.vbat.validity), (unsigned long)a.vbat.age_ms);
  put("\"acq\":{\"running\":%s,\"restarts\":%lu,\"missed\":%lu,\"cycles\":%lu,\"cal\":%u,",
      a.running ? "true" : "false", (unsigned long)a.restarts, (unsigned long)a.missed_halves,
      (unsigned long)a.cycles, a.cal_version);
  if (a.ct.validity == ACQ_VALID)
    put("\"ct\":{\"state\":\"VALID\",\"ma\":%ld,\"dc_raw\":%u},", (long)a.ct.value, a.ct.raw);
  else
    put("\"ct\":{\"state\":\"%s\",\"ma\":null,\"dc_raw\":null},", Acquisition_ValidityName(a.ct.validity));
  put("\"ac\":[");
  for (unsigned int i = 0; i < ACQ_AC_CHANNELS; i++) {
    if (a.ac_v[i].validity == ACQ_VALID)
      put("%s{\"state\":\"VALID\",\"v_x10\":%ld,\"rms_raw\":%u}", i ? "," : "", (long)a.ac_v[i].value, a.ac_v[i].raw);
    else if (a.ac_v[i].validity == ACQ_NOT_FITTED)
      put("%s{\"state\":\"NOT_FITTED\",\"v_x10\":null,\"rms_raw\":null}", i ? "," : "");
    else  /* rms_raw: diagnostic counts of (P-N), shown even when the value is not trusted */
      put("%s{\"state\":\"%s\",\"v_x10\":null,\"rms_raw\":%u}", i ? "," : "",
          Acquisition_ValidityName(a.ac_v[i].validity), a.ac_v[i].raw);
  }
  put("],");
  const AcqPq *q = &a.pq;
  put("\"pq\":{\"freq_mhz\":");
  if (q->freq_valid) put("%lu", (unsigned long)q->freq_mhz); else put("null");
  put(",\"synced\":%s,\"ref_line\":%d,", q->synced ? "true" : "false", (int)q->ref_line);
  if (q->v_valid) {
    put("\"v_thd_x10\":%u,\"v_crest_x100\":%u,\"v_h_x10\":[", q->v_thd_x10, q->v_crest_x100);
    for (unsigned int k = 0; k < ACQ_PQ_HARMONICS - 1U; k++) put("%s%u", k ? "," : "", q->v_h_x10[k]);
    put("],");
  } else put("\"v_thd_x10\":null,\"v_crest_x100\":null,\"v_h_x10\":null,");
  if (q->i_valid) {
    put("\"i_thd_x10\":%u,\"i_crest_x100\":%u,\"i_h_x10\":[", q->i_thd_x10, q->i_crest_x100);
    for (unsigned int k = 0; k < ACQ_PQ_HARMONICS - 1U; k++) put("%s%u", k ? "," : "", q->i_h_x10[k]);
    put("],");
  } else put("\"i_thd_x10\":null,\"i_crest_x100\":null,\"i_h_x10\":null,");
  if (q->p_valid) put("\"p_dw\":%ld,\"s_dva\":%ld,\"q_dvar\":%ld,", (long)q->p_dw, (long)q->s_dva, (long)q->q_dvar);
  else put("\"p_dw\":null,\"s_dva\":null,\"q_dvar\":null,");
  if (q->pf_valid) put("\"pf_x1000\":%d,\"dpf_x1000\":%d,", q->pf_x1000, q->dpf_x1000);
  else put("\"pf_x1000\":null,\"dpf_x1000\":null,");
  put("\"energy_mwh\":%lu,\"ct_reversed\":%s,", (unsigned long)(q->energy_uwh / 1000U), q->ct_reversed ? "true" : "false");
  put("\"events\":{\"active\":%s,\"supply_min_dv\":%u,\"supply_max_dv\":%u,\"sag\":%lu,\"swell\":%lu,\"interruption\":%lu,",
      q->events_active ? "true" : "false", q->supply_min_dv, q->supply_max_dv,
      (unsigned long)q->sag_count, (unsigned long)q->swell_count, (unsigned long)q->interruption_count);
  if (q->last.type != PQ_EV_NONE)
    put("\"last\":{\"type\":\"%s\",\"start_ms\":%lu,\"duration_ms\":%lu,\"extreme_dv\":%u,\"active\":%s,\"open_ended\":%s}},",
        Acquisition_EventName(q->last.type), (unsigned long)q->last.start_ms, (unsigned long)q->last.duration_ms,
        q->last.extreme_dv, q->last.active ? "true" : "false", q->last.open_ended ? "true" : "false");
  else put("\"last\":null},");
  put("\"contact\":[\"%s\",\"%s\",\"%s\",\"%s\"]}},", Acquisition_ContactName(q->contact[0]),
      Acquisition_ContactName(q->contact[1]), Acquisition_ContactName(q->contact[2]), Acquisition_ContactName(q->contact[3]));

  put("\"amc_diag\":[%u,%u,%u,%u],", pin(DIAG_S1_GPIO_Port, DIAG_S1_Pin), pin(DIAG_S2_GPIO_Port, DIAG_S2_Pin),
      pin(DIAG_S3_GPIO_Port, DIAG_S3_Pin), pin(DIAG_S4_GPIO_Port, DIAG_S4_Pin));

  put("\"flash\":[");
  for (unsigned int i = 0; i < 2U; i++)
    put("%s{\"ok\":%s,\"jedec\":\"%02X %02X %02X\"}", i ? "," : "", flashOk[i] ? "true" : "false",
        flashId[i][0], flashId[i][1], flashId[i][2]);
  put("],");

  StStatus st;
  StConfig sc;
  Storage_GetStatus(&st);
  Storage_GetConfig(&sc);
  put("\"storage\":{\"state\":\"%s\",\"event_seq\":%lu,\"history_records\":%lu,\"energy_saved_mwh\":%lu,"
      "\"dropped\":%lu,\"write_errors\":%lu,\"config\":{\"seq\":%lu,\"amc\":%d,\"ct_phase_cdeg\":%d},"
      "\"amc_active\":%s,\"rtc_s\":%lu,\"rtc_kept\":%s},",
      Storage_StateName(st.state), (unsigned long)st.event_seq, (unsigned long)st.history_records,
      (unsigned long)(st.energy_uwh_saved / 1000U), (unsigned long)st.dropped, (unsigned long)st.write_errors,
      (unsigned long)sc.seq, sc.amc_fitted == 0xFFU ? -1 : (int)sc.amc_fitted, (int)sc.ct_phase_cdeg,
      Acquisition_AmcFitted() ? "true" : "false", (unsigned long)Boot_RtcSeconds(), Boot_RtcKept() ? "true" : "false");

  RemoteStatus rs;
  RemoteLink_GetStatus(&rs);
  const StRemoteConfig *rc = &sc.remote;
  put("\"remote\":{\"enabled\":%s,\"key_set\":%s,\"state\":\"%s\",\"peer\":\"%u.%u.%u.%u\",\"connected_s\":%lu,"
      "\"connects\":%lu,\"auth_failures\":%lu,\"sweeps\":%lu,\"commands\":%lu,\"rejected\":%lu,"
      "\"ip_first\":%u,\"ip_last\":%u,\"port\":%u,\"fixed_ip\":\"%u.%u.%u.%u\",\"interval_s\":%u,"
      "\"min_gap_s\":%u,\"max_per_min\":%u},",
      rc->enabled ? "true" : "false", rc->key[0] ? "true" : "false", RemoteLink_StateName(rs.state),
      (unsigned)(rs.peer_ip & 255U), (unsigned)((rs.peer_ip >> 8) & 255U), (unsigned)((rs.peer_ip >> 16) & 255U),
      (unsigned)(rs.peer_ip >> 24), (unsigned long)rs.connected_s, (unsigned long)rs.connects,
      (unsigned long)rs.auth_failures, (unsigned long)rs.sweeps, (unsigned long)rs.commands, (unsigned long)rs.rejected,
      rc->ip_first, rc->ip_last, rc->port, (unsigned)(rc->fixed_ip & 255U), (unsigned)((rc->fixed_ip >> 8) & 255U),
      (unsigned)((rc->fixed_ip >> 16) & 255U), (unsigned)(rc->fixed_ip >> 24), rc->interval_s, rc->min_gap_s,
      rc->max_per_min);
  if (Boot_TimeValid()) put("\"time\":{\"valid\":true,\"unix\":%lu},", (unsigned long)Boot_UnixNow());
  else put("\"time\":{\"valid\":false,\"unix\":null},");

  DebugLogStats ls = {0};
  (void)DebugLog_GetStats(&ls);
  put("\"sys\":{\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,", (unsigned)xPortGetFreeHeapSize(),
      (unsigned)xPortGetMinimumEverFreeHeapSize(), (unsigned)configTOTAL_HEAP_SIZE);
  put("\"reset_cause\":\"%s\",\"reset_count\":%lu,\"safe_mode\":%s,\"watchdog\":{\"boot_gap_ms\":%lu,\"max_gap_ms\":%lu,\"phase\":\"%s\",\"lost\":%s%s%s,\"hb_ms\":{",
      Boot_ResetCause(), (unsigned long)Boot_ResetCount(), Boot_SafeMode() ? "true" : "false",
      (unsigned long)Supervisor_BootGapMs(), (unsigned long)Boot_WdtMaxGapMs(), Supervisor_Phase(),
      Supervisor_LostTask() ? "\"" : "", Supervisor_LostTask() ? Supervisor_LostTask() : "null",
      Supervisor_LostTask() ? "\"" : "");
  for (int t = 0; t < (int)SUP_TASK_COUNT; t++) {
    uint32_t age = Supervisor_HeartbeatAgeMs((SupTask)t);
    if (age == UINT32_MAX) put("%s\"%s\":null", t ? "," : "", Supervisor_TaskName((SupTask)t));
    else put("%s\"%s\":%lu", t ? "," : "", Supervisor_TaskName((SupTask)t), (unsigned long)age);
  }
  put("}},\"rtc_kept\":%s,\"i2c_recoveries\":%lu,", Boot_RtcKept() ? "true" : "false",
      (unsigned long)I2cBus_RecoveryCount());
  put("\"log\":{\"queued\":%lu,\"high\":%lu,\"dropped\":%lu,\"dma_start_err\":%lu,\"dma_xfer_err\":%lu},",
      (unsigned long)ls.queued_bytes, (unsigned long)ls.high_water_bytes, (unsigned long)ls.dropped_messages,
      (unsigned long)ls.dma_start_errors, (unsigned long)ls.dma_transfer_errors);
  UBaseType_t count = uxTaskGetSystemState(taskStatus, MAX_TASKS, NULL);
  put("\"tasks\":[");
  for (UBaseType_t i = 0; i < count; i++)
    put("%s{\"name\":\"%s\",\"prio\":%u,\"stack_free\":%u}", i ? "," : "", taskStatus[i].pcTaskName,
        (unsigned)taskStatus[i].uxCurrentPriority, (unsigned)taskStatus[i].usStackHighWaterMark);
  put("]}}");
}

static error_t sendJson(HttpConnection *connection, const char *body, size_t length)
{
  connection->response.statusCode = 200;
  connection->response.contentType = "application/json";
  connection->response.chunkedEncoding = FALSE;
  connection->response.contentLength = length;
  connection->response.noCache = TRUE;
  error_t error = httpWriteHeader(connection);
  if (!error) error = httpWriteStream(connection, body, length);
  if (!error) error = httpCloseStream(connection);
  return error;
}

static error_t sendJsonStatus(HttpConnection *connection, uint_t code, const char *body)
{
  connection->response.statusCode = code;
  connection->response.contentType = "application/json";
  connection->response.chunkedEncoding = FALSE;
  connection->response.contentLength = strlen(body);
  connection->response.noCache = TRUE;
  error_t error = httpWriteHeader(connection);
  if (!error) error = httpWriteStream(connection, body, strlen(body));
  if (!error) error = httpCloseStream(connection);
  return error;
}

/* Minimal JSON field readers for small, flat request bodies. Strings may not
 * contain escapes or quotes (tokens, passwords, enum names). */
static const char *findKey(const char *body, const char *key)
{
  char pattern[24];
  (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(body, pattern);
  if (p == NULL) return NULL;
  p += strlen(pattern);
  while (*p == ' ') p++;
  return *p == ':' ? p + 1 : NULL;
}

static bool jsonString(const char *body, const char *key, char *out, size_t size)
{
  const char *p = findKey(body, key);
  if (p == NULL) return false;
  while (*p == ' ') p++;
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"' && *p != '\\' && n + 1 < size) out[n++] = *p++;
  out[n] = '\0';
  return *p == '"';
}

static bool jsonUint(const char *body, const char *key, uint32_t *out)
{
  const char *p = findKey(body, key);
  if (p == NULL) return false;
  while (*p == ' ') p++;
  if (*p < '0' || *p > '9') return false;
  uint32_t v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000U) v = v * 10U + (uint32_t)(*p++ - '0');
  *out = v;
  return true;
}

static bool readBody(HttpConnection *connection, char *buf, size_t size)
{
  size_t len = connection->request.contentLength;
  if (connection->request.chunkedEncoding || len == 0U || len >= size) return false;
  size_t got = 0;
  if (httpReadStream(connection, buf, len, &got, HTTP_FLAG_WAIT_ALL) != NO_ERROR || got != len) return false;
  buf[len] = '\0';
  return true;
}

static error_t handleLogin(HttpConnection *connection, const char *body)
{
  char pw[32], token[WEB_AUTH_TOKEN_LEN + 1], reply[64];
  if (!jsonString(body, "password", pw, sizeof(pw))) return sendJsonStatus(connection, 400, "{\"error\":\"password missing\"}");
  WebAuthResult r = WebAuth_Login(pw, token);
  if (r == AUTH_LOCKED) return sendJsonStatus(connection, 429, "{\"error\":\"too many attempts\"}");
  if (r != AUTH_OK) return sendJsonStatus(connection, 403, "{\"error\":\"bad password\"}");
  (void)snprintf(reply, sizeof(reply), "{\"csrf\":\"%s\"}", token);
  return sendJsonStatus(connection, 200, reply);
}

static error_t handleRelay(HttpConnection *connection, const char *body)
{
  char type[16], pw[32], reply[96];
  uint32_t mask = 0, duration = 0;
  if (!jsonString(body, "type", type, sizeof(type)) || !jsonUint(body, "channel_mask", &mask))
    return sendJsonStatus(connection, 400, "{\"error\":\"invalid\",\"reason\":\"INVALID\"}");
  (void)jsonUint(body, "duration_s", &duration);
  ControlRequest req = { .channel_mask = (uint8_t)mask, .duration_s = (uint16_t)duration, .source = SRC_WEB };
  if (strcmp(type, "SET_ON") == 0) req.type = CTRL_SET_ON;
  else if (strcmp(type, "SET_OFF") == 0) req.type = CTRL_SET_OFF;
  else if (strcmp(type, "POWER_CYCLE") == 0) req.type = CTRL_POWER_CYCLE;
  else if (strcmp(type, "CANCEL_CYCLE") == 0) req.type = CTRL_CANCEL_CYCLE;
  else return sendJsonStatus(connection, 400, "{\"error\":\"invalid\",\"reason\":\"INVALID\"}");
  if (mask > 0xFFU || duration > 0xFFFFU) return sendJsonStatus(connection, 400, "{\"error\":\"invalid\",\"reason\":\"INVALID\"}");
  /* Commands that remove power from equipment need the password again. */
  if (req.type == CTRL_SET_OFF || req.type == CTRL_POWER_CYCLE) {
    if (!jsonString(body, "password", pw, sizeof(pw))) return sendJsonStatus(connection, 403, "{\"error\":\"password required\"}");
    WebAuthResult a = WebAuth_CheckPassword(pw);
    if (a == AUTH_LOCKED) return sendJsonStatus(connection, 429, "{\"error\":\"too many attempts\"}");
    if (a != AUTH_OK) return sendJsonStatus(connection, 403, "{\"error\":\"bad password\"}");
  }
  uint32_t id = 0;
  CtrlSubmitResult s = Control_Submit(&req, &id);
  uint_t code = 409;
  if (s == CTRL_SUBMIT_ACCEPTED) {
    (void)snprintf(reply, sizeof(reply), "{\"request_id\":%lu}", (unsigned long)id);
    return sendJsonStatus(connection, 200, reply);
  }
  if (s == CTRL_SUBMIT_REJECTED_INVALID) code = 400;
  else if (s == CTRL_SUBMIT_BUSY || s == CTRL_SUBMIT_NOT_READY) code = 503;
  (void)snprintf(reply, sizeof(reply), "{\"error\":\"rejected\",\"reason\":\"%s\"}", Control_SubmitResultName(s));
  return sendJsonStatus(connection, code, reply);
}

static error_t handleOutcome(HttpConnection *connection)
{
  const char *q = connection->request.queryString;
  if (strncmp(q, "id=", 3) != 0) return sendJsonStatus(connection, 400, "{\"error\":\"id missing\"}");
  uint32_t id = 0;
  for (const char *p = q + 3; *p >= '0' && *p <= '9' && id < 100000000U; p++) id = id * 10U + (uint32_t)(*p - '0');
  CtrlOutcome o;
  if (!Control_GetOutcome(id, &o)) return sendJsonStatus(connection, 404, "{\"error\":\"unknown or expired\"}");
  char reply[96];
  (void)snprintf(reply, sizeof(reply), "{\"state\":\"%s\",\"reason\":%s%s%s}", Control_OutcomeName(o.state),
      o.reason ? "\"" : "", o.reason ? o.reason : "null", o.reason ? "\"" : "");
  return sendJsonStatus(connection, 200, reply);
}

/* GET /api/waveform: one mains cycle, built in the shared JSON buffer (jsonMutex held). */
static void buildWaveform(void)
{
  static AcqWaveform w;            /* static: too large for the HTTP connection stack */
  Acquisition_GetWaveform(&w);
  jsonLen = 0U; jsonOverflow = false;
  put("{\"seq\":%lu,\"ref_line\":%d,\"fs_mhz\":%lu,\"v_uv_per_count\":%lu,\"i_ua_per_count\":%lu,\"v\":[",
      (unsigned long)w.seq, (int)w.ref_line, (unsigned long)(w.fs * 1000.0f),
      (unsigned long)(w.v_per_count * 1e6f), (unsigned long)(w.a_per_count * 1e6f));
  for (unsigned int k = 0; k < 128U; k++) put("%s%d", k ? "," : "", w.v[k]);
  put("],\"i\":[");
  for (unsigned int k = 0; k < 128U; k++) put("%s%d", k ? "," : "", w.i[k]);
  put("]}");
}

/* GET /api/history[?n=N]: binary, little endian. 16-byte header
 * "PQH1", u8 version 1, u8 fields 9, u16 entry size 18, u16 period s, u16 count,
 * u32 uptime s at the end of the newest entry; then count x HistEntry, oldest first. */
static error_t sendHistory(HttpConnection *connection)
{
  uint32_t total = History_Count(), n = total;
  const char *q = connection->request.queryString;
  if (strncmp(q, "n=", 2) == 0) {
    uint32_t v = 0;
    for (const char *p = q + 2; *p >= '0' && *p <= '9' && v < 100000U; p++) v = v * 10U + (uint32_t)(*p - '0');
    if (v < n) n = v;
  }
  uint32_t first = total - n;
  uint8_t hdr[16] = { 'P', 'Q', 'H', '1', 1U, 9U, (uint8_t)sizeof(HistEntry), 0U,
      (uint8_t)HIST_PERIOD_S, (uint8_t)(HIST_PERIOD_S >> 8), (uint8_t)n, (uint8_t)(n >> 8) };
  uint32_t endS = History_NewestEndS();
  memcpy(&hdr[12], &endS, 4);
  connection->response.statusCode = 200;
  connection->response.contentType = "application/octet-stream";
  connection->response.chunkedEncoding = FALSE;
  connection->response.contentLength = sizeof(hdr) + n * sizeof(HistEntry);
  connection->response.noCache = TRUE;
  error_t error = httpWriteHeader(connection);
  if (!error) error = httpWriteStream(connection, hdr, sizeof(hdr));
  static HistEntry chunk[32];      /* static: HTTP stack is small; one sender at a time (mutex) */
  for (uint32_t i = 0; !error && i < n; ) {
    uint32_t c = 0;
    while (c < 32U && i < n) {
      if (!History_Get(first + i, &chunk[c])) memset(&chunk[c], 0x80, sizeof(HistEntry));
      c++; i++;
    }
    error = httpWriteStream(connection, chunk, c * sizeof(HistEntry));
  }
  if (!error) error = httpCloseStream(connection);
  return error;
}

/* GET /api/events[?n=N]: newest first (jsonMutex held). */
static void buildEvents(const char *q)
{
  static StEvent ev[40];
  uint32_t want = 30;
  if (strncmp(q, "n=", 2) == 0) { want = (uint32_t)atoi(q + 2); if (want == 0U || want > 40U) want = 40U; }
  uint32_t n = Storage_ReadEvents(ev, want);
  jsonLen = 0U; jsonOverflow = false;
  put("{\"rtc_s\":%lu,\"uptime_s\":%lu,\"events\":[", (unsigned long)Boot_RtcSeconds(),
      (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ));
  for (uint32_t i = 0; i < n; i++)
    put("%s{\"seq\":%lu,\"rtc_s\":%lu,\"up_s\":%lu,\"type\":%u,\"a\":%ld,\"b\":%ld,\"c\":%ld}", i ? "," : "",
        (unsigned long)ev[i].seq, (unsigned long)ev[i].rtc_s, (unsigned long)ev[i].uptime_s, ev[i].type,
        (long)ev[i].a, (long)ev[i].b, (long)ev[i].c);
  put("]}");
}

/* History aggregation into time buckets, streamed as binary "PQH2":
 * 16-byte header "PQH2", u8 version 2, u8 fields 9, u8 entry size 22, u8 0, u16 bucket s,
 * u16 0, u32 RTC now (s since 2000); then entries u32 bucket end RTC + 9 x i16, oldest first.
 * Chunked transfer (length unknown in advance). */
typedef struct {
  HttpConnection *c;
  uint32_t bucket, key;
  int32_t sum[9]; uint16_t cnt[9]; int16_t mn, mx;
  uint8_t out[22 * 24]; uint32_t outLen;
  error_t err;
} HistAgg;

static void aggFlushBucket(HistAgg *g)
{
  bool any = false;
  for (int f = 0; f < 9; f++) if (g->cnt[f]) any = true;
  if (any) {
    uint8_t *o = &g->out[g->outLen];
    uint32_t end = (g->key + 1U) * g->bucket;
    memcpy(o, &end, 4);
    for (int f = 0; f < 9; f++) {
      int16_t v = HIST_MISSING;
      if (g->cnt[f]) {
        if (f == 1) v = g->mn; else if (f == 2) v = g->mx;
        else { int32_t a = g->sum[f] / (int32_t)g->cnt[f]; v = (int16_t)a; }
      }
      memcpy(o + 4 + 2 * f, &v, 2);
    }
    g->outLen += 22U;
    if (g->outLen + 22U > sizeof(g->out)) {
      if (!g->err) g->err = httpWriteStream(g->c, g->out, g->outLen);
      g->outLen = 0;
    }
  }
  memset(g->sum, 0, sizeof(g->sum)); memset(g->cnt, 0, sizeof(g->cnt));
  g->mn = INT16_MAX; g->mx = INT16_MIN;
}

static void aggAdd(HistAgg *g, uint32_t rtc, const HistEntry *e)
{
  uint32_t key = rtc / g->bucket;
  if (key != g->key) { if (g->key != UINT32_MAX) aggFlushBucket(g); g->key = key; }
  const int16_t *v = &e->v_avg_dv;
  for (int f = 0; f < 9; f++) {
    if (v[f] == HIST_MISSING) continue;
    if (f == 1) { if (v[f] < g->mn) g->mn = v[f]; }
    else if (f == 2) { if (v[f] > g->mx) g->mx = v[f]; }
    else g->sum[f] += v[f];
    g->cnt[f]++;
  }
}

static void aggCallback(uint32_t rtc_s, const HistEntry *e, void *ctx) { aggAdd((HistAgg *)ctx, rtc_s, e); }

static error_t sendHistory2(HttpConnection *connection)
{
  static HistAgg g;              /* static: HTTP stack is small; one sender at a time (mutex) */
  const char *q = connection->request.queryString;
  uint32_t span = 86400U, bucket = 60U;
  if (strstr(q, "range=7d")) { span = 7U * 86400U; bucket = 600U; }
  else if (strstr(q, "range=30d")) { span = 30U * 86400U; bucket = 1800U; }
  else if (strstr(q, "range=90d")) { span = 90U * 86400U; bucket = 3600U; }
  uint32_t now = Boot_RtcSeconds();
  memset(&g, 0, sizeof(g));
  g.c = connection; g.bucket = bucket; g.key = UINT32_MAX; g.mn = INT16_MAX; g.mx = INT16_MIN;
  uint8_t hdr[16] = { 'P', 'Q', 'H', '2', 2U, 9U, 22U, 0U, (uint8_t)bucket, (uint8_t)(bucket >> 8), 0U, 0U };
  memcpy(&hdr[12], &now, 4);
  connection->response.statusCode = 200;
  connection->response.contentType = "application/octet-stream";
  connection->response.chunkedEncoding = TRUE;
  connection->response.noCache = TRUE;
  error_t error = httpWriteHeader(connection);
  if (!error) error = httpWriteStream(connection, hdr, sizeof(hdr));
  if (!error) {
    StStatus st;
    Storage_GetStatus(&st);
    uint32_t from = now > span ? now - span : 0U;
    if (st.state == ST_STATE_READY && st.history_records > 0U) {
      (void)Storage_ReadHistory(from, aggCallback, &g);
    } else {
      /* no flash history: the RAM ring (uptime based) mapped onto RTC time */
      uint32_t total = History_Count(), upNow = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ), endS = History_NewestEndS();
      HistEntry e;
      for (uint32_t i = 0; i < total; i++) {
        if (!History_Get(i, &e)) break;
        uint32_t entEnd = endS - (total - 1U - i) * HIST_PERIOD_S;
        uint32_t rtc = now - (upNow - entEnd);
        if (rtc >= from) aggAdd(&g, rtc, &e);
      }
    }
    if (g.key != UINT32_MAX) aggFlushBucket(&g);
    error = g.err;
    if (!error && g.outLen) error = httpWriteStream(connection, g.out, g.outLen);
  }
  if (!error) error = httpCloseStream(connection);
  return error;
}

/* GET /api/fw/status (jsonMutex held). */
static void buildFwStatus(void)
{
  FotaStatus f;
  Fota_GetStatus(&f);
  jsonLen = 0U; jsonOverflow = false;
  put("{\"running\":{\"version\":\"%lu.%lu.%lu\",\"build\":\"%s\",\"dev\":%s,\"confirmed\":%s},",
      (unsigned long)(f.run_version >> 16), (unsigned long)((f.run_version >> 8) & 255U), (unsigned long)(f.run_version & 255U),
      f.run_build, f.run_dev ? "true" : "false", f.state == SSW_BR_TRIAL ? "false" : "true");
  put("\"bootloader\":%s,\"boot\":{\"state\":\"%s\",\"last_result\":\"%s\",\"last_version\":\"%lu.%lu.%lu\",\"trial_boots\":%lu,\"trial_max\":%u},",
      f.bootloader_present ? "true" : "false", Fota_StateName(f.state), Fota_ResultName(f.result),
      (unsigned long)(f.result_version >> 16), (unsigned long)((f.result_version >> 8) & 255U), (unsigned long)(f.result_version & 255U),
      (unsigned long)f.trial_boots, SSW_TRIAL_MAX_BOOTS);
  put("\"backup\":{\"present\":%s,\"size\":%lu},", f.backup_size ? "true" : "false", (unsigned long)f.backup_size);
  put("\"slot\":{\"state\":\"%s\",\"version\":", Fota_SlotName(f.slot));
  if (f.slot == FOTA_SLOT_VALID)
    put("\"%lu.%lu.%lu\",\"build\":\"%s\",\"size\":%lu,", (unsigned long)(f.slot_version >> 16),
        (unsigned long)((f.slot_version >> 8) & 255U), (unsigned long)(f.slot_version & 255U), f.slot_build, (unsigned long)f.slot_size);
  else put("null,\"build\":null,\"size\":null,");
  put("\"received\":%lu,\"reason\":", (unsigned long)f.received);
  if (f.slot_reason) put("\"%s\"", f.slot_reason); else put("null");
  put("},\"reboot_pending\":%s}", f.reboot_pending ? "true" : "false");
}

/* POST /api/fw/upload?token=<session>: raw image body (Content-Length), written to the
 * staging slot while it arrives, then verified. One upload at a time. */
static error_t handleUpload(HttpConnection *connection)
{
  static uint8_t chunk[1024];
  static volatile bool busy;
  char token[WEB_AUTH_TOKEN_LEN + 1] = { 0 };
  const char *q = strstr(connection->request.queryString, "token=");
  if (q) { strncpy(token, q + 6, WEB_AUTH_TOKEN_LEN); token[WEB_AUTH_TOKEN_LEN] = 0; }
  if (!WebAuth_CheckToken(token)) return sendJsonStatus(connection, 401, "{\"error\":\"sign in required\"}");
  if (connection->request.chunkedEncoding) return sendJsonStatus(connection, 411, "{\"error\":\"Content-Length required\"}");
  taskENTER_CRITICAL();
  bool mine = !busy; if (mine) busy = true;
  taskEXIT_CRITICAL();
  if (!mine) return sendJsonStatus(connection, 409, "{\"error\":\"upload in progress\"}");
  const char *reason = NULL;
  uint32_t len = (uint32_t)connection->request.contentLength;
  bool ok = Fota_UploadBegin(len, &reason);
  uint32_t got = 0;
  while (ok && got < len) {
    size_t n = 0, want = len - got > sizeof(chunk) ? sizeof(chunk) : len - got;
    if (httpReadStream(connection, chunk, want, &n, HTTP_FLAG_WAIT_ALL) != NO_ERROR || n == 0U) { reason = "connection lost"; ok = false; break; }
    if (!Fota_UploadWrite(chunk, (uint32_t)n)) { reason = "flash write error"; ok = false; break; }
    got += (uint32_t)n;
  }
  if (ok) ok = Fota_UploadEnd(&reason);
  else Fota_UploadAbort(reason ? reason : "rejected");
  busy = false;
  char reply[160];
  if (ok) {
    (void)snprintf(reply, sizeof(reply), "{\"state\":\"VALID\",\"size\":%lu}", (unsigned long)len);
    return sendJsonStatus(connection, 200, reply);
  }
  (void)snprintf(reply, sizeof(reply), "{\"error\":\"rejected\",\"reason\":\"%s\"}", reason ? reason : "error");
  return sendJsonStatus(connection, 422, reply);
}

/* POST /api/fw/apply | /api/fw/rollback {token, password} */
static error_t handleFwAction(HttpConnection *connection, const char *body, bool rollback)
{
  char pw[24];
  if (!jsonString(body, "password", pw, sizeof(pw))) return sendJsonStatus(connection, 400, "{\"error\":\"password required\"}");
  WebAuthResult a = WebAuth_CheckPassword(pw);
  memset(pw, 0, sizeof(pw));
  if (a == AUTH_LOCKED) return sendJsonStatus(connection, 429, "{\"error\":\"too many attempts\"}");
  if (a != AUTH_OK) return sendJsonStatus(connection, 403, "{\"error\":\"bad password\"}");
  const char *reason = NULL;
  bool ok = rollback ? Fota_RequestRollback(&reason) : Fota_RequestInstall(&reason);
  char reply[140];
  if (ok) return sendJsonStatus(connection, 200, "{\"rebooting\":true}");
  (void)snprintf(reply, sizeof(reply), "{\"error\":\"rejected\",\"reason\":\"%s\"}", reason ? reason : "error");
  return sendJsonStatus(connection, 409, reply);
}

/* POST /api/remote {token, password, enabled, ip_first, ip_last, port, fixed_ip, interval_s,
 * min_gap_s, max_per_min, key?, clear_key?}: remote link settings (docs/remote-link-protocol.md).
 * key omitted or "" keeps the current key; the key is never sent back. */
static error_t handleRemote(HttpConnection *connection, const char *body)
{
  char pw[32], ip[20], key[ST_REMOTE_KEY_MAX + 2];
  if (!jsonString(body, "password", pw, sizeof(pw))) return sendJsonStatus(connection, 403, "{\"error\":\"password required\"}");
  WebAuthResult a = WebAuth_CheckPassword(pw);
  if (a == AUTH_LOCKED) return sendJsonStatus(connection, 429, "{\"error\":\"too many attempts\"}");
  if (a != AUTH_OK) return sendJsonStatus(connection, 403, "{\"error\":\"bad password\"}");
  static StConfig c;               /* static: HTTP stack is small; one writer at a time (mutex) */
  Storage_GetConfig(&c);
  StRemoteConfig *r = &c.remote;
  uint32_t en = 0, first = 0, last = 0, port = 0, iv = 0, gap = 0, maxm = 0, clr = 0;
  if (!jsonUint(body, "enabled", &en) || !jsonUint(body, "ip_first", &first) || !jsonUint(body, "ip_last", &last) ||
      !jsonUint(body, "port", &port) || !jsonUint(body, "interval_s", &iv) || !jsonUint(body, "min_gap_s", &gap) ||
      !jsonUint(body, "max_per_min", &maxm))
    return sendJsonStatus(connection, 400, "{\"error\":\"missing field\"}");
  if (en > 1U || first < 1U || first > 254U || last < first || last > 254U || port < 1U || port > 65535U ||
      iv < 1U || iv > 60U || gap > 255U || maxm < 1U || maxm > 60U)
    return sendJsonStatus(connection, 400, "{\"error\":\"value out of range\"}");
  Ipv4Addr fixed = IPV4_UNSPECIFIED_ADDR;
  if (jsonString(body, "fixed_ip", ip, sizeof(ip)) && ip[0] != '\0' && strcmp(ip, "0.0.0.0") != 0 &&
      ipv4StringToAddr(ip, &fixed) != NO_ERROR)
    return sendJsonStatus(connection, 400, "{\"error\":\"bad fixed IP\"}");
  if (jsonString(body, "key", key, sizeof(key)) && key[0] != '\0') {
    size_t n = strlen(key);
    if (n < 16U || n > ST_REMOTE_KEY_MAX) return sendJsonStatus(connection, 400, "{\"error\":\"key must be 16-64 characters\"}");
    for (size_t i = 0; i < n; i++)
      if (key[i] < 0x21 || key[i] > 0x7E) return sendJsonStatus(connection, 400, "{\"error\":\"key: printable ASCII, no spaces\"}");
    memset(r->key, 0, sizeof(r->key));
    memcpy(r->key, key, n);
  } else if (jsonUint(body, "clear_key", &clr) && clr == 1U) {
    memset(r->key, 0, sizeof(r->key));
  }
  r->enabled = (uint8_t)en; r->ip_first = (uint8_t)first; r->ip_last = (uint8_t)last; r->port = (uint16_t)port;
  r->interval_s = (uint8_t)iv; r->min_gap_s = (uint8_t)gap; r->max_per_min = (uint8_t)maxm; r->fixed_ip = fixed;
  memset(key, 0, sizeof(key)); memset(pw, 0, sizeof(pw));
  if (!Storage_SetConfig(&c)) return sendJsonStatus(connection, 503, "{\"error\":\"storage not ready\"}");
  Storage_LogEvent(ST_EV_CONFIG, (int32_t)c.amc_fitted, (int32_t)c.ct_phase_cdeg, 1, 0);
  return sendJsonStatus(connection, 200, "{}");
}

bool WebServer_WithJson(WebJsonKind kind, WebJsonSink sink, void *ctx)
{
  if (jsonMutex == NULL || xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  if (kind == WEB_JSON_STATUS) buildStatus();
  else if (kind == WEB_JSON_WAVEFORM) buildWaveform();
  else buildFwStatus();
  bool ok = !jsonOverflow;
  if (ok) sink(json, jsonLen, ctx);
  xSemaphoreGive(jsonMutex);
  return ok;
}

static error_t requestCallback(HttpConnection *connection, const char_t *uri)
{
  if (strncmp(uri, "/api/", 5) != 0) {
    /* Static page from res.c. No caching: after a firmware update the browser must load
     * the page that matches the new API (CycloneTCP keeps this flag for the static reply). */
    connection->response.noCache = TRUE;
    return ERROR_NOT_FOUND;
  }
  bool get = strcmp(connection->request.method, "GET") == 0;
  bool post = strcmp(connection->request.method, "POST") == 0;
  if (get && strcmp(uri, "/api/status") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    buildStatus();
    error_t error = jsonOverflow ? httpSendErrorResponse(connection, 500, "Status too large")
                                 : sendJson(connection, json, jsonLen);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  if (get && strcmp(uri, "/api/waveform") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    buildWaveform();
    error_t error = jsonOverflow ? httpSendErrorResponse(connection, 500, "Too large") : sendJson(connection, json, jsonLen);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  if (get && strcmp(uri, "/api/events") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    buildEvents(connection->request.queryString);
    error_t error = jsonOverflow ? httpSendErrorResponse(connection, 500, "Too large") : sendJson(connection, json, jsonLen);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  if (get && strcmp(uri, "/api/history") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    error_t error = strstr(connection->request.queryString, "range=") ? sendHistory2(connection) : sendHistory(connection);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  if (get && strcmp(uri, "/api/fw/status") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    buildFwStatus();
    error_t error = jsonOverflow ? httpSendErrorResponse(connection, 500, "Too large") : sendJson(connection, json, jsonLen);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  if (post && strcmp(uri, "/api/fw/upload") == 0) return handleUpload(connection);
  if (get && strcmp(uri, "/api/relay/outcome") == 0) return handleOutcome(connection);
  if (post && (strcmp(uri, "/api/login") == 0 || strcmp(uri, "/api/logout") == 0 || strcmp(uri, "/api/relay") == 0 ||
               strcmp(uri, "/api/fw/apply") == 0 || strcmp(uri, "/api/fw/rollback") == 0 ||
               strcmp(uri, "/api/session") == 0 || strcmp(uri, "/api/remote") == 0)) {
    char body[400];
    if (!readBody(connection, body, sizeof(body))) return sendJsonStatus(connection, 400, "{\"error\":\"bad body\"}");
    if (strcmp(uri, "/api/login") == 0) return handleLogin(connection, body);
    /* Session token in the body: a cross-site form cannot read or forge it (CSRF). */
    char token[WEB_AUTH_TOKEN_LEN + 1];
    if (!jsonString(body, "token", token, sizeof(token)) || !WebAuth_CheckToken(token))
      return sendJsonStatus(connection, 401, "{\"error\":\"sign in required\"}");
    if (strcmp(uri, "/api/logout") == 0) { WebAuth_Logout(token); return sendJsonStatus(connection, 200, "{}"); }
    if (strcmp(uri, "/api/session") == 0) return sendJsonStatus(connection, 200, "{}");   /* token still valid */
    if (strcmp(uri, "/api/fw/apply") == 0) return handleFwAction(connection, body, false);
    if (strcmp(uri, "/api/fw/rollback") == 0) return handleFwAction(connection, body, true);
    if (strcmp(uri, "/api/remote") == 0) return handleRemote(connection, body);
    return handleRelay(connection, body);
  }
  /* Network settings and firmware update come in later phases. */
  return httpSendErrorResponse(connection, 501, "Not available on this firmware");
}

bool WebServer_Start(NetInterface *interface)
{
  jsonMutex = xSemaphoreCreateMutexStatic(&jsonMutexStorage);
  if (jsonMutex == NULL) return false;
  WebAuth_Init();
  for (unsigned int i = 0; i < 2U; i++)
    flashOk[i] = W25Q128_ReadId((W25Q128_Chip)i, flashId[i]) == W25Q128_OK;

  httpServerGetDefaultSettings(&settings);
  settings.interface = interface;
  settings.port = HTTP_PORT;
  settings.maxConnections = HTTP_SERVER_MAX_CONNECTIONS;
  settings.connections = connections;
  strcpy(settings.rootDirectory, "/");
  strcpy(settings.defaultDocument, "index.html");
  settings.requestCallback = requestCallback;
  error_t error = httpServerInit(&context, &settings);
  if (!error) error = httpServerStart(&context);
  if (error) {
    (void)DebugLog_Printf("WEB: start error %ld\r\n", (long)error);
    return false;
  }
  (void)DebugLog_Printf("WEB: HTTP server on port 80 (status, sign-in, relay commands)\r\n");
  return true;
}
