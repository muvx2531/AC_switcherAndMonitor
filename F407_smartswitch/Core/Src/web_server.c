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
#include "w25q128.h"
#include "boot.h"
#include "supervisor.h"
#include "control.h"
#include "web_auth.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define JSON_SIZE 3072U
#define MAX_TASKS 16U

static HttpServerSettings settings;
static HttpServerContext context;
static HttpConnection connections[HTTP_SERVER_MAX_CONNECTIONS];

/* One JSON builder shared by both HTTP connection tasks, guarded by a mutex. */
static SemaphoreHandle_t jsonMutex;
static StaticSemaphore_t jsonMutexStorage;
static char json[JSON_SIZE];
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
  put("{\"fw\":{\"build\":\"%s\",\"uptime_ms\":%lu},", build, (unsigned long)uptimeMs);

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
      "\"effective\":[\"%s\",\"%s\",\"%s\",\"%s\"],\"glitches\":%lu,\"supply_releases\":%lu,\"boot_note\":\"%s\"},",
      c.available ? "true" : "false", c.state_known ? "true" : "false", c.coil_supply ? "true" : "false",
      c.cycle_remaining_s[0], c.cycle_remaining_s[1], c.cycle_remaining_s[2], c.cycle_remaining_s[3],
      eff[c.effective[0]], eff[c.effective[1]], eff[c.effective[2]], eff[c.effective[3]],
      (unsigned long)c.glitch_count, (unsigned long)c.supply_releases, c.boot_note ? c.boot_note : "");

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
  put("]},");

  put("\"amc_diag\":[%u,%u,%u,%u],", pin(DIAG_S1_GPIO_Port, DIAG_S1_Pin), pin(DIAG_S2_GPIO_Port, DIAG_S2_Pin),
      pin(DIAG_S3_GPIO_Port, DIAG_S3_Pin), pin(DIAG_S4_GPIO_Port, DIAG_S4_Pin));

  put("\"flash\":[");
  for (unsigned int i = 0; i < 2U; i++)
    put("%s{\"ok\":%s,\"jedec\":\"%02X %02X %02X\"}", i ? "," : "", flashOk[i] ? "true" : "false",
        flashId[i][0], flashId[i][1], flashId[i][2]);
  put("],");

  DebugLogStats ls = {0};
  (void)DebugLog_GetStats(&ls);
  put("\"sys\":{\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,", (unsigned)xPortGetFreeHeapSize(),
      (unsigned)xPortGetMinimumEverFreeHeapSize(), (unsigned)configTOTAL_HEAP_SIZE);
  put("\"reset_cause\":\"%s\",\"reset_count\":%lu,\"safe_mode\":%s,\"watchdog\":{\"phase\":\"%s\",\"lost\":%s%s%s,\"hb_ms\":{",
      Boot_ResetCause(), (unsigned long)Boot_ResetCount(), Boot_SafeMode() ? "true" : "false", Supervisor_Phase(),
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

static error_t requestCallback(HttpConnection *connection, const char_t *uri)
{
  if (strncmp(uri, "/api/", 5) != 0) return ERROR_NOT_FOUND;  /* static page from res.c */
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
  if (get && strcmp(uri, "/api/relay/outcome") == 0) return handleOutcome(connection);
  if (post && (strcmp(uri, "/api/login") == 0 || strcmp(uri, "/api/logout") == 0 || strcmp(uri, "/api/relay") == 0)) {
    char body[256];
    if (!readBody(connection, body, sizeof(body))) return sendJsonStatus(connection, 400, "{\"error\":\"bad body\"}");
    if (strcmp(uri, "/api/login") == 0) return handleLogin(connection, body);
    /* Session token in the body: a cross-site form cannot read or forge it (CSRF). */
    char token[WEB_AUTH_TOKEN_LEN + 1];
    if (!jsonString(body, "token", token, sizeof(token)) || !WebAuth_CheckToken(token))
      return sendJsonStatus(connection, 401, "{\"error\":\"sign in required\"}");
    if (strcmp(uri, "/api/logout") == 0) { WebAuth_Logout(token); return sendJsonStatus(connection, 200, "{}"); }
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
