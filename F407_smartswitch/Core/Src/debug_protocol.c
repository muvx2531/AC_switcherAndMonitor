#include "debug_protocol.h"
#include "debug_log.h"
#include "pca9536.h"
#include "acquisition.h"
#include "control.h"
#include "boot.h"
#include "storage.h"
#include <stdlib.h>
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* USART6 RX circular DMA. RX-complete and error callbacks are registered on the
 * USART6 handle (USE_HAL_UART_REGISTER_CALLBACKS); no global HAL symbols. */
#define RX_SIZE 512U
#define LINE_SIZE 48U
static uint8_t rxData[RX_SIZE];
static UART_HandleTypeDef *serial;
static TaskHandle_t worker;
static volatile uint32_t rxWraps;
static volatile bool rxFailed;

static void rxCompleteCallback(UART_HandleTypeDef *uart);
static void errorCallback(UART_HandleTypeDef *uart);

/* ADC is owned by the Acquisition service: read its 1 s VBAT mean from the snapshot. */
static bool readBattery(uint16_t *raw, uint32_t *mv)
{
  AcqSnapshot a;
  Acquisition_GetSnapshot(&a);
  if (a.vbat.validity != ACQ_VALID) return false;
  *raw = a.vbat.raw;
  *mv = (uint32_t)a.vbat.value;
  return true;
}

static int hexDigit(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void response(const char *body)
{
  uint8_t sum = 0;
  for (const char *p = body; *p; ++p) sum = (uint8_t)(sum + (uint8_t)*p);
  char frame[96];
  int n = snprintf(frame, sizeof(frame), "@%s*%02X\r\n", body, sum);
  if (n > 0 && (size_t)n < sizeof(frame)) (void)DebugLog_Write(frame, (size_t)n);
}

/* Submit and wait (max ~1 s) for the outcome. NULL on success, else a reason. */
static const char *submitAndWait(CtrlType type, uint8_t mask)
{
  if (mask == 0U) return NULL;
  ControlRequest req = { .type = type, .channel_mask = mask, .duration_s = 0, .source = SRC_SERIAL };
  uint32_t id;
  CtrlSubmitResult s = Control_Submit(&req, &id);
  if (s != CTRL_SUBMIT_ACCEPTED) return Control_SubmitResultName(s);
  CtrlOutcome o = { .state = OUT_PENDING };
  for (unsigned int i = 0; i < 100U; i++) {
    if (!Control_GetOutcome(id, &o) || o.state != OUT_PENDING) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (o.state == OUT_APPLIED) return NULL;
  return o.reason != NULL ? o.reason : Control_OutcomeName(o.state);
}

static const char *applyPattern(uint8_t value)
{
  const char *reason = submitAndWait(CTRL_SET_OFF, value & CONTROL_ALL_CHANNELS);
  if (reason == NULL) reason = submitAndWait(CTRL_SET_ON, (uint8_t)(~value) & CONTROL_ALL_CHANNELS);
  return reason;
}

static void handleLine(char *line)
{
  size_t len = strlen(line);
  if (len < 10U || line[0] != '@' || line[len - 3U] != '*') return;
  int hi = hexDigit(line[len - 2U]), lo = hexDigit(line[len - 1U]);
  if (hi < 0 || lo < 0 || hexDigit(line[1]) < 0 || hexDigit(line[2]) < 0 || line[3] != ',') return;
  char seq[3] = {line[1], line[2], 0};
  uint8_t sum = 0;
  for (size_t i = 1; i < len - 3U; ++i) sum = (uint8_t)(sum + (uint8_t)line[i]);
  char body[80];
  if (sum != (uint8_t)((hi << 4) | lo)) {
    (void)snprintf(body, sizeof(body), "%s,ERR,CHECKSUM", seq); response(body); return;
  }
  line[len - 3U] = '\0';
  if (strcmp(line + 4, "BATV") == 0) {
    uint16_t raw;
    uint32_t mv;
    if (!readBattery(&raw, &mv)) {
      (void)snprintf(body, sizeof(body), "%s,ERR,ADC", seq); response(body); return;
    }
    (void)snprintf(body, sizeof(body), "%s,BATV,%u,%lu,3000", seq,
        (unsigned)raw, (unsigned long)mv);
    response(body); return;
  }
  if (strcmp(line + 4, "STFORMAT") == 0) {  /* erase + format the storage partitions of chip 1 */
    Storage_RequestFormat();
    (void)snprintf(body, sizeof(body), "%s,STFORMAT,QUEUED", seq); response(body); return;
  }
  if (strncmp(line + 4, "AMC,", 4) == 0 || strncmp(line + 4, "CTPH,", 5) == 0) {
    /* board config, effective after the next reset: AMC,0|1|D  CTPH,<0.01 degree> */
    StConfig c;
    Storage_GetConfig(&c);
    if (line[4] == 'A') c.amc_fitted = line[8] == 'D' ? 0xFFU : (line[8] == '1' ? 1U : 0U);
    else c.ct_phase_cdeg = (int16_t)atoi(line + 9);
    bool ok = Storage_SetConfig(&c);
    if (ok) Storage_LogEvent(ST_EV_CONFIG, (int32_t)c.amc_fitted, (int32_t)c.ct_phase_cdeg, 0, 0);
    (void)snprintf(body, sizeof(body), "%s,CFG,%s", seq, ok ? "QUEUED" : "ERR"); response(body); return;
  }
  if (strcmp(line + 4, "FLSCAN") == 0) {    /* read-only whole-chip flash scan, results in the log */
    extern volatile uint32_t flashTestRequest;
    flashTestRequest = 2U;
    (void)snprintf(body, sizeof(body), "%s,FLSCAN,STARTED", seq); response(body); return;
  }
  if (strcmp(line + 4, "CLRSAFE") == 0) {   /* clear the boot-loop counter (S6-5) */
    Boot_ClearResetCount();
    (void)snprintf(body, sizeof(body), "%s,CLRSAFE,OK", seq); response(body); return;
  }
  if (strcmp(line + 4, "BACKUP") == 0) {
    uint16_t raw;
    uint32_t mv;
    if (!readBattery(&raw, &mv)) {
      (void)snprintf(body, sizeof(body), "%s,ERR,ADC", seq); response(body); return;
    }
    (void)snprintf(body, sizeof(body), "%s,BACKUP,%u,%u,%u,%u,%u", seq,
        (unsigned)raw,
        (unsigned)HAL_GPIO_ReadPin(Batt_status_GPIO_Port, Batt_status_Pin),
        (unsigned)HAL_GPIO_ReadPin(Batt_FAULT_GPIO_Port, Batt_FAULT_Pin),
        (unsigned)HAL_GPIO_ReadPin(Vin_Detect_GPIO_Port, Vin_Detect_Pin),
        (unsigned)HAL_GPIO_ReadPin(eFuse_iLim_GPIO_Port, eFuse_iLim_Pin));
    response(body); return;
  }
  bool set = false;
  int value = 0;
  if (strcmp(line + 4, "GET") == 0) { }
  else if (strlen(line + 4) == 5U && memcmp(line + 4, "SET,", 4) == 0 &&
      (value = hexDigit(line[8])) >= 0) set = true;
  else { (void)snprintf(body, sizeof(body), "%s,ERR,COMMAND", seq); response(body); return; }

  if (set) {
    /* Phase 2: the serial port is an adapter of the Control service. Bit = 1 means
     * coil on (load OFF): one SET_OFF for those bits and one SET_ON for the rest. */
    const char *reason = applyPattern((uint8_t)value);
    if (reason != NULL) {
      (void)snprintf(body, sizeof(body), "%s,ERR,%s", seq, reason); response(body); return;
    }
  }
  PCA9536_Registers regs;  /* read-only */
  I2cBusResult r = PCA9536_ReadRegisters(&regs);
  if (r != I2C_BUS_OK) {
    (void)snprintf(body, sizeof(body), "%s,ERR,I2C,%u", seq, (unsigned)r); response(body); return;
  }
  (void)snprintf(body, sizeof(body), "%s,OK,%X,%X,%X", seq,
      regs.output & 0x0fU, regs.input & 0x0fU, regs.config & 0x0fU);
  response(body);
}

static void rxTask(void *argument)
{
  (void)argument;
  char line[LINE_SIZE];
  size_t used = 0;
  bool collecting = false;
  uint32_t consumed = 0;
  for (;;) {
    if (rxFailed) { (void)DebugLog_Printf("protocol: UART RX failure; reboot required\r\n"); vTaskDelete(NULL); }
    taskENTER_CRITICAL();
    uint32_t wraps = rxWraps;
    uint32_t remaining = __HAL_DMA_GET_COUNTER(serial->hdmarx);
    taskEXIT_CRITICAL();
    uint32_t produced = wraps * RX_SIZE + ((RX_SIZE - remaining) % RX_SIZE);
    uint32_t count = produced - consumed;
    if (count > RX_SIZE) { consumed = produced; used = 0; collecting = false; }
    else {
      while (consumed != produced) {
        char c = (char)rxData[consumed % RX_SIZE];
        consumed++;
        if (c == '@') { collecting = true; used = 0; line[used++] = c; }
        else if (!collecting) { }
        else if (c == '\n') {
          if (used && line[used - 1U] == '\r') used--;
          line[used] = '\0'; handleLine(line); used = 0; collecting = false;
        }
        else if (used < sizeof(line) - 1U) line[used++] = c;
        else { used = 0; collecting = false; }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool DebugProtocol_Start(UART_HandleTypeDef *uart)
{
  if (!uart || uart->Instance != USART6 || !uart->hdmarx ||
      uart->hdmarx->Init.Mode != DMA_CIRCULAR || __get_IPSR() || worker) return false;
  /* HAL accepts registration only while USART6 TX is READY, so call this before
   * the debug logger starts transmitting. Register before RX DMA starts. */
  if (HAL_UART_RegisterCallback(uart, HAL_UART_RX_COMPLETE_CB_ID, rxCompleteCallback) != HAL_OK ||
      HAL_UART_RegisterCallback(uart, HAL_UART_ERROR_CB_ID, errorCallback) != HAL_OK) return false;
  serial = uart;
  if (HAL_UART_Receive_DMA(serial, rxData, sizeof(rxData)) != HAL_OK) { serial = NULL; return false; }
  if (xTaskCreate(rxTask, "debugRx", 512, NULL, tskIDLE_PRIORITY + 25U, &worker) != pdPASS) {
    (void)HAL_UART_AbortReceive(serial); serial = NULL; return false;
  }
  return true;
}
static void rxCompleteCallback(UART_HandleTypeDef *uart)
{ if (uart == serial) rxWraps++; }
/* USART6 has one error callback for both directions; TX DMA errors also land here. */
static void errorCallback(UART_HandleTypeDef *uart)
{ if (uart == serial) rxFailed = true; }
