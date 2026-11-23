#include "web_server.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "http/http_server.h"
#include "debug_log.h"
#include "eth_probe.h"
#include "pca9536.h"
#include "adc_raw.h"
#include "w25q128.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define JSON_SIZE 2048U
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
  put("\"net\":{\"link\":%s,\"mode\":\"%s\",\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"mac\":\"%s\",\"phy_id\":\"%04X:%04X\"},",
      n.link ? "true" : "false", n.dhcp ? "dhcp" : (n.addressValid ? "static" : "none"),
      n.ip, n.mask, n.gw, n.mac, n.phyId1, n.phyId2);

  /* Read-only: no relay state is changed by the web in this phase. */
  PCA9536_Registers r;
  I2cBusResult rr = PCA9536_ReadRegisters(&r);
  if (rr == I2C_BUS_OK)
    put("\"relay\":{\"ok\":true,\"err\":0,\"output\":%u,\"input\":%u,\"config\":%u},", r.output, r.input, r.config);
  else
    put("\"relay\":{\"ok\":false,\"err\":%u,\"output\":null,\"input\":null,\"config\":null},", (unsigned)rr);

  put("\"power\":{\"vin_detect\":%u,\"efuse_ilim\":%u,\"chrg\":%u,\"fault\":%u},",
      pin(Vin_Detect_GPIO_Port, Vin_Detect_Pin), pin(eFuse_iLim_GPIO_Port, eFuse_iLim_Pin),
      pin(Batt_status_GPIO_Port, Batt_status_Pin), pin(Batt_FAULT_GPIO_Port, Batt_FAULT_Pin));

  uint16_t raw;
  if (AdcRaw_GetVbat(&raw))
    put("\"vbat\":{\"ok\":true,\"raw\":%u,\"mv_est\":%lu},", raw, (unsigned long)AdcRaw_VbatEstimateMv(raw));
  else
    put("\"vbat\":{\"ok\":false,\"raw\":null,\"mv_est\":null},");

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

static error_t requestCallback(HttpConnection *connection, const char_t *uri)
{
  if (strncmp(uri, "/api/", 5) != 0) return ERROR_NOT_FOUND;  /* static page from res.c */
  if (strcmp(uri, "/api/status") == 0 && strcmp(connection->request.method, "GET") == 0) {
    if (xSemaphoreTake(jsonMutex, pdMS_TO_TICKS(1000)) != pdTRUE)
      return httpSendErrorResponse(connection, 503, "Busy");
    buildStatus();
    error_t error = jsonOverflow ? httpSendErrorResponse(connection, 500, "Status too large")
                                 : sendJson(connection, json, jsonLen);
    xSemaphoreGive(jsonMutex);
    return error;
  }
  /* Login, relay, network and firmware endpoints come in later phases. */
  return httpSendErrorResponse(connection, 501, "Not available on this firmware");
}

bool WebServer_Start(NetInterface *interface)
{
  jsonMutex = xSemaphoreCreateMutexStatic(&jsonMutexStorage);
  if (jsonMutex == NULL) return false;
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
  (void)DebugLog_Printf("WEB: HTTP server on port 80 (read-only status)\r\n");
  return true;
}
