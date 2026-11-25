/* Local UI: status LED patterns and OLED status pages (see local_ui.h,
 * docs/local-ui-status.md). Reads the other services' snapshots; relay commands only from
 * the button control mode, through Control_Submit (SRC_LOCAL). */
#include "local_ui.h"
#include "acquisition.h"
#include "boot.h"
#include "control.h"
#include "debug_log.h"
#include "eth_probe.h"
#include "fota.h"
#include "fw_version.h"
#include "i2c_bus.h"
#include "power.h"
#include "ssw_oled.h"
#include "storage.h"
#include "supervisor.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS          100U
#define PAGE_MS          4000U
#define REDRAW_MS        1000U
#define DIM_AFTER_MS     (5UL * 60UL * 1000UL)
#define OLED_RETRY_MS    10000U
#define CONTRAST_FULL    0xCFU
#define CONTRAST_DIM     0x01U
#define MAX_ALERTS       8U

static TaskHandle_t task;
static StaticTask_t tcb;
static StackType_t stack[512] __attribute__((section(".ccmram")));

/* Snapshots are large (AcqSnapshot ~0.4 KB): static, used by this task only. */
static ControlSnapshot ctl;
static PowerSnapshot pwr;
static AcqSnapshot acq;
static EthInfo eth;
static FotaStatus fw;
static StStatus sto;
static uint8_t fb[SSW_OLED_FB_SIZE];

/* ---- status --------------------------------------------------------------- */
typedef enum { LV_BOOT, LV_OK, LV_TRIAL, LV_WARN, LV_LOWPWR, LV_FAULT, LV_UPDATE } Level;

typedef struct { const char *title; char l1[40]; char l2[40]; } Alert;

static Level level;
static Alert alerts[MAX_ALERTS];
static unsigned alertCount;

static void verStr(char *o, size_t n, uint32_t v)
{
  (void)snprintf(o, n, "%lu.%lu.%lu", (unsigned long)(v >> 16), (unsigned long)((v >> 8) & 255U), (unsigned long)(v & 255U));
}

static void vbatStr(char *o, size_t n)
{
  if (acq.vbat.validity == ACQ_VALID) (void)snprintf(o, n, "%ld.%01ldV", (long)(acq.vbat.value / 1000), (long)((acq.vbat.value % 1000) / 100));
  else (void)snprintf(o, n, "?V");
}

static Alert *addAlert(const char *title)
{
  if (alertCount >= MAX_ALERTS) return NULL;
  Alert *a = &alerts[alertCount++];
  a->title = title; a->l1[0] = 0; a->l2[0] = 0;
  return a;
}

static void evaluate(void)
{
  Control_GetSnapshot(&ctl);
  Power_GetSnapshot(&pwr);
  Acquisition_GetSnapshot(&acq);
  EthProbe_GetInfo(&eth);
  Fota_GetStatus(&fw);
  Storage_GetStatus(&sto);

  bool safe = Boot_SafeMode();
  bool fault = false, warn = false;
  alertCount = 0;
  Alert *a;
  char v[12];

  if (safe && (a = addAlert("SAFE MODE")) != NULL) {
    fault = true;
    (void)snprintf(a->l1, sizeof a->l1, "Too many resets");
    (void)snprintf(a->l2, sizeof a->l2, "Serial CLRSAFE clears");
  }
  if ((!ctl.available || !ctl.state_known) && (a = addAlert("RELAY STATE UNKNOWN")) != NULL) {
    fault = true;
    (void)snprintf(a->l1, sizeof a->l1, "PCA9536 / I2C error");
    (void)snprintf(a->l2, sizeof a->l2, "Commands refused");
  }
  if (!safe && !acq.running && (a = addAlert("MEASUREMENT FAULT")) != NULL) {
    fault = true;
    (void)snprintf(a->l1, sizeof a->l1, "ADC sampling stopped");
  }
  if (pwr.mode == POWER_BACKUP && (a = addAlert("BACKUP - NO 24V")) != NULL) {
    warn = true; vbatStr(v, sizeof v);
    (void)snprintf(a->l1, sizeof a->l1, "Battery %s", v);
    (void)snprintf(a->l2, sizeof a->l2, "OFF refused, LAN off");
  }
  if (pwr.mode == POWER_LOW_POWER && (a = addAlert("LOW POWER")) != NULL) {
    vbatStr(v, sizeof v);
    (void)snprintf(a->l1, sizeof a->l1, "Battery %s", v);
    (void)snprintf(a->l2, sizeof a->l2, "No 24V, LAN off");
  }
  if (!safe && pwr.mode == POWER_MAINS && eth.available && eth.powered && !eth.link &&
      (a = addAlert("NO LAN CABLE")) != NULL) {
    warn = true;
    (void)snprintf(a->l1, sizeof a->l1, "Check Ethernet cable");
  }
  if (!safe && (sto.state == ST_STATE_FAULT || sto.state == ST_STATE_UNFORMATTED) &&
      (a = addAlert("STORAGE PROBLEM")) != NULL) {
    warn = true;
    (void)snprintf(a->l1, sizeof a->l1, sto.state == ST_STATE_FAULT ? "Flash not answering" : "Flash not formatted");
    (void)snprintf(a->l2, sizeof a->l2, "Logs not saved");
  }
  if (acq.running && acq.pq.ct_reversed && (a = addAlert("CT BACKWARDS")) != NULL) {
    warn = true;
    (void)snprintf(a->l1, sizeof a->l1, "Power reads negative");
    (void)snprintf(a->l2, sizeof a->l2, "Turn the CT around");
  }
  if (acq.running && acq.pq.last.active && (a = addAlert("SUPPLY EVENT")) != NULL) {
    warn = true;
    (void)snprintf(a->l1, sizeof a->l1, "%s", Acquisition_EventName(acq.pq.last.type));
    (void)snprintf(a->l2, sizeof a->l2, "Extreme %u.%uV", acq.pq.last.extreme_dv / 10U, acq.pq.last.extreme_dv % 10U);
  }
  bool trial = fw.state == SSW_BR_TRIAL;
  if (trial && (a = addAlert("FIRMWARE TRIAL")) != NULL) {
    verStr(v, sizeof v, fw.run_version);
    (void)snprintf(a->l1, sizeof a->l1, "New FW %s %lu/3", v, (unsigned long)fw.trial_boots);
    (void)snprintf(a->l2, sizeof a->l2, "Confirms after 60 s");
  }

  if (fw.reboot_pending) level = LV_UPDATE;
  else if (fault) level = LV_FAULT;
  else if (pwr.mode == POWER_LOW_POWER) level = LV_LOWPWR;
  else if (warn) level = LV_WARN;
  else if (strcmp(Supervisor_Phase(), "boot") == 0) level = LV_BOOT;
  else if (trial) level = LV_TRIAL;
  else level = LV_OK;
}

/* ---- LED ------------------------------------------------------------------ */
/* One bit per 100 ms slot, bit 0 first; frame of `len` slots. */
static void ledPattern(Level lv, uint32_t *bits, unsigned *len)
{
  *len = 20U;
  switch (lv) {
    case LV_BOOT:   *bits = 0xFFFFFU; break;              /* steady on */
    case LV_OK:     *bits = 0x00001U; break;              /* 1 flash / 2 s */
    case LV_WARN:   *bits = 0x00009U; break;              /* 2 flashes */
    case LV_TRIAL:  *bits = 0x00049U; break;              /* 3 flashes */
    case LV_FAULT:  *bits = 0x11111U; break;              /* fast: 100 on / 300 off */
    case LV_UPDATE: *bits = 0x55555U; break;              /* flicker, like the bootloader */
    case LV_LOWPWR: *bits = 0x00001U; *len = 50U; break;  /* 1 flash / 5 s */
    default:        *bits = 0x00001U; break;
  }
}

/* ---- OLED ----------------------------------------------------------------- */
static bool oledOk, oledLogged, dim;
static TickType_t oledRetryAt, lastInput;
static unsigned page;
static TickType_t pageSince, lastDraw;

static bool oledWrite(uint8_t ctrl, const uint8_t *data, uint16_t len)
{
  if (I2cBus_Lock() != I2C_BUS_OK) return false;          /* lock released between blocks */
  I2cBusResult r = I2cBus_WriteBlockLocked(SSW_OLED_ADDR, ctrl, data, len);
  I2cBus_Unlock();
  return r == I2C_BUS_OK;
}

static void oledFailed(void)
{
  oledOk = false;
  oledRetryAt = xTaskGetTickCount() + pdMS_TO_TICKS(OLED_RETRY_MS);
  if (!oledLogged) { oledLogged = true; (void)DebugLog_Printf("UI: OLED not responding, retry every 10 s\r\n"); }
}

static const char *tag(void)
{
  switch (level) {
    case LV_BOOT: return "BOOT";
    case LV_OK: return " OK ";
    case LV_TRIAL: return "TRIAL";
    case LV_WARN: return "WARN";
    case LV_LOWPWR: return "LOW";
    case LV_FAULT: return Boot_SafeMode() ? "SAFE" : "FAULT";
    default: return "UPD";
  }
}

static void drawRelays(unsigned line)
{
  for (unsigned ch = 0; ch < CONTROL_CHANNELS; ch++) {
    char t[8];
    const char *st = !ctl.available || !ctl.state_known ? "?" :
                     ctl.cycle_remaining_s[ch] ? "CYC" :
                     ctl.effective[ch] == EFF_ON ? "ON" : ctl.effective[ch] == EFF_OFF ? "OFF" : "?";
    (void)snprintf(t, sizeof t, "%u:%s", ch + 1U, st);
    SswOled_Text(fb, ch * 32U, line, t, false);
  }
}

static void drawMain(void)
{
  char t[48];
  unsigned x = SswOled_Text(fb, 1, 0, tag(), true);
  const char *mode = pwr.mode == POWER_MAINS ? "MAINS" : pwr.mode == POWER_BACKUP ? "BATTERY" : "LOW PWR";
  SswOled_Text(fb, x + 5U, 0, mode, false);
  SswOled_Text(fb, SSW_OLED_W - 6U * (unsigned)strlen(FW_VERSION_STR), 0, FW_VERSION_STR, false);
  drawRelays(1);

  int r = acq.pq.ref_line;
  char vs[16] = "---V", is[16] = "-.--A", fs[16] = "";
  if (r >= 0 && r < (int)ACQ_AC_CHANNELS && acq.ac_v[r].validity == ACQ_VALID)
    (void)snprintf(vs, sizeof vs, "%ld.%ldV", (long)(acq.ac_v[r].value / 10), (long)(acq.ac_v[r].value % 10));
  else if (!Acquisition_AmcFitted()) (void)snprintf(vs, sizeof vs, "noV");
  if (acq.ct.validity == ACQ_VALID)
    (void)snprintf(is, sizeof is, "%ld.%02ldA", (long)(acq.ct.value / 1000), (long)((acq.ct.value % 1000) / 10));
  if (acq.pq.freq_valid)
    (void)snprintf(fs, sizeof fs, "%lu.%luHz", (unsigned long)(acq.pq.freq_mhz / 1000U), (unsigned long)((acq.pq.freq_mhz % 1000U) / 100U));
  (void)snprintf(t, sizeof t, "%s %s %s", vs, is, fs);
  SswOled_Text(fb, 0, 2, t, false);

  if (Boot_SafeMode()) (void)snprintf(t, sizeof t, "LAN off (safe mode)");
  else if (!eth.available) (void)snprintf(t, sizeof t, "LAN starting");
  else if (!eth.powered) (void)snprintf(t, sizeof t, "LAN off (battery)");
  else if (!eth.link) (void)snprintf(t, sizeof t, "LAN: no cable");
  else if (!eth.addressValid) (void)snprintf(t, sizeof t, "LAN: waiting for IP");
  else (void)snprintf(t, sizeof t, "IP %s%s", eth.ip, eth.dhcp ? "" : " S");
  SswOled_Text(fb, 0, 3, t, false);
}

static void drawInfo(void)
{
  char t[48];
  (void)snprintf(t, sizeof t, "MAC %s", eth.mac[0] ? eth.mac : "--");
  SswOled_Text(fb, 0, 0, t, false);
  if (acq.pq.p_valid) {
    char pf[16] = "PF-.--", thd[16] = "";
    int pfv = acq.pq.pf_x1000;
    if (acq.pq.pf_valid) (void)snprintf(pf, sizeof pf, "PF%s%d.%02d", pfv < 0 ? "-" : "", abs(pfv) / 1000, (abs(pfv) % 1000) / 10);
    if (acq.pq.v_valid) (void)snprintf(thd, sizeof thd, "THD%u.%u%%", acq.pq.v_thd_x10 / 10U, acq.pq.v_thd_x10 % 10U);
    (void)snprintf(t, sizeof t, "%ldW %s %s", (long)(acq.pq.p_dw / 10), pf, thd);
  } else (void)snprintf(t, sizeof t, "Power: no voltage");
  SswOled_Text(fb, 0, 1, t, false);
  uint64_t wh = Acquisition_EnergyUwh() / 1000000ULL;
  (void)snprintf(t, sizeof t, "Energy %lu.%02lu kWh", (unsigned long)(wh / 1000U), (unsigned long)((wh % 1000U) / 10U));
  SswOled_Text(fb, 0, 2, t, false);
  uint32_t s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
  (void)snprintf(t, sizeof t, "Up %lud %02lu:%02lu", (unsigned long)(s / 86400U), (unsigned long)((s / 3600U) % 24U), (unsigned long)((s / 60U) % 60U));
  SswOled_Text(fb, 0, 3, t, false);
}

static void drawAlert(const Alert *a)
{
  SswOled_Text(fb, 1, 0, a->title, true);
  SswOled_Text(fb, 0, 1, a->l1, false);
  SswOled_Text(fb, 0, 2, a->l2, false);
  drawRelays(3);
}

static void drawUpdate(void)
{
  char t[48], v[12];
  SswOled_Text(fb, 1, 0, "FIRMWARE UPDATE", true);
  if (fw.state == SSW_BR_ROLLBACK) (void)snprintf(t, sizeof t, "Restoring previous");
  else { verStr(v, sizeof v, fw.slot_version); (void)snprintf(t, sizeof t, "Installing %s", v); }
  SswOled_Text(fb, 0, 1, t, false);
  SswOled_Text(fb, 0, 2, "Rebooting...", false);
  SswOled_Text(fb, 0, 3, "DO NOT POWER OFF", false);
}

/* Pages: main, each alert, info. */
static unsigned pageCount(void) { return 2U + alertCount; }

static void render(void)
{
  SswOled_Clear(fb);
  if (level == LV_UPDATE) { drawUpdate(); return; }
  unsigned n = pageCount();
  if (page >= n) page = 0;
  if (page == 0U) drawMain();
  else if (page <= alertCount) drawAlert(&alerts[page - 1U]);
  else drawInfo();
}

/* ---- buttons and control mode ---------------------------------------------- */
/* Owner decisions 2026-11-25 (D1-D3): hold SW1 or SW2 for 5 s to enter control mode;
 * SW1 steps CH1 -> CH4 -> EXIT -> CH1; hold SW2 for 2 s to toggle the selected relay
 * ON <-> OFF (no power-cycle from the buttons) or to leave on EXIT; 10 s without a press
 * returns to the status pages. While in control mode web/serial commands are refused. */
#define ENTER_HOLD_MS    5000U
#define ACT_HOLD_MS      2000U
#define HOLD_SHOW_MS     700U       /* show the "keep holding" screen after this */
#define SHORT_PRESS_MS   700U
#define CONTROL_IDLE_MS  10000U
#define MSG_MS           3000U
#define SEL_EXIT         CONTROL_CHANNELS

typedef struct { bool down, used; uint32_t heldMs; } Button;
static Button sw1, sw2;
static bool controlMode, armed;
static unsigned sel;
static TickType_t lastAct, msgUntil;
static char msg[32];
static uint32_t pendingId;
static TickType_t pendingSince;
static unsigned pendingCh;
static CtrlType pendingType;

static void readButton(Button *b, GPIO_TypeDef *port, uint16_t pin)
{
  bool d = HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_RESET;   /* active low, RC debounced */
  if (d) b->heldMs += TICK_MS; else { b->heldMs = 0; b->used = false; }
  b->down = d;
}

static void setMsg(TickType_t now, const char *t)
{
  (void)snprintf(msg, sizeof msg, "%s", t);
  msgUntil = now + pdMS_TO_TICKS(MSG_MS);
}

static void leaveControl(TickType_t now)
{
  controlMode = false;
  Control_LocalLock(false);
  page = 0; pageSince = now; lastDraw = 0;
  (void)DebugLog_Printf("UI: control mode left\r\n");
}

static void act(TickType_t now)
{
  if (sel == SEL_EXIT) { leaveControl(now); return; }
  Control_GetSnapshot(&ctl);
  ControlRequest r = { .type = ctl.effective[sel] == EFF_ON ? CTRL_SET_OFF : CTRL_SET_ON,
                       .channel_mask = (uint8_t)(1U << sel), .duration_s = 0, .source = SRC_LOCAL };
  uint32_t id = 0;
  CtrlSubmitResult res = Control_Submit(&r, &id);
  char t[32];
  if (res == CTRL_SUBMIT_ACCEPTED) {
    pendingId = id; pendingSince = now; pendingCh = sel; pendingType = r.type;
    (void)snprintf(t, sizeof t, "CH%u -> %s ...", sel + 1U, r.type == CTRL_SET_OFF ? "OFF" : "ON");
  } else if (res == CTRL_SUBMIT_REJECTED_NO_COIL_SUPPLY) (void)snprintf(t, sizeof t, "Refused: no 24V");
  else if (res == CTRL_SUBMIT_REJECTED_STATE_UNKNOWN) (void)snprintf(t, sizeof t, "Refused: state unknown");
  else (void)snprintf(t, sizeof t, "Refused: %s", Control_SubmitResultName(res));
  setMsg(now, t);
  (void)DebugLog_Printf("UI: button %s CH%u -> %s\r\n", r.type == CTRL_SET_OFF ? "OFF" : "ON", sel + 1U,
      Control_SubmitResultName(res));
}

static void pollOutcome(TickType_t now)
{
  if (pendingId == 0U) return;
  CtrlOutcome o;
  if (!Control_GetOutcome(pendingId, &o)) { pendingId = 0; return; }
  if (o.state == OUT_PENDING && now - pendingSince < pdMS_TO_TICKS(3000)) return;
  char t[32];
  if (o.state == OUT_APPLIED) (void)snprintf(t, sizeof t, "CH%u %s: done", pendingCh + 1U, pendingType == CTRL_SET_OFF ? "OFF" : "ON");
  else (void)snprintf(t, sizeof t, "CH%u: %s", pendingCh + 1U, o.reason ? o.reason : Control_OutcomeName(o.state));
  setMsg(now, t);
  pendingId = 0;
}

/* Returns a short press (released before SHORT_PRESS_MS, not consumed). */
static bool buttons(TickType_t now)
{
  static bool was1, was2;
  static uint32_t last1, last2;
  readButton(&sw1, SW1_GPIO_Port, SW1_Pin);
  readButton(&sw2, SW2_GPIO_Port, SW2_Pin);
  bool short1 = was1 && !sw1.down && last1 < SHORT_PRESS_MS && !controlMode;
  bool short2 = was2 && !sw2.down && last2 < SHORT_PRESS_MS && !controlMode;
  bool press1 = sw1.down && !was1;
  was1 = sw1.down; was2 = sw2.down;
  if (sw1.down) last1 = sw1.heldMs;
  if (sw2.down) last2 = sw2.heldMs;

  if (!controlMode) {
    Button *h = sw1.heldMs >= sw2.heldMs ? &sw1 : &sw2;
    if (h->heldMs >= ENTER_HOLD_MS && !h->used && oledOk) {
      controlMode = true; armed = false; sel = 0; lastAct = now; msg[0] = 0; msgUntil = now;
      sw1.used = sw2.used = true;
      Control_LocalLock(true);
      (void)DebugLog_Printf("UI: control mode entered (web/serial commands refused)\r\n");
    }
    return short1 || short2;
  }

  Control_LocalLock(true);                                   /* refresh while in the mode */
  if (!armed) { if (!sw1.down && !sw2.down) armed = true; else return false; }
  if (sw1.down || sw2.down) lastAct = now;
  if (press1) { sel = (sel + 1U) % (CONTROL_CHANNELS + 1U); msgUntil = now; }
  if (sw2.down && !sw2.used && sw2.heldMs >= ACT_HOLD_MS) { sw2.used = true; act(now); }
  pollOutcome(now);
  if (controlMode && !sw1.down && !sw2.down && now - lastAct >= pdMS_TO_TICKS(CONTROL_IDLE_MS)) leaveControl(now);
  return false;
}

static void drawRelaysSel(unsigned line, int selected)
{
  for (unsigned ch = 0; ch < CONTROL_CHANNELS; ch++) {
    char t[8];
    const char *st = !ctl.state_known ? "?" : ctl.cycle_remaining_s[ch] ? "CYC" :
                     ctl.effective[ch] == EFF_ON ? "ON" : ctl.effective[ch] == EFF_OFF ? "OFF" : "?";
    (void)snprintf(t, sizeof t, "%u:%s", ch + 1U, st);
    SswOled_Text(fb, ch * 32U + 1U, line, t, (int)ch == selected);
  }
}

static void drawHold(void)
{
  uint32_t h = sw1.heldMs > sw2.heldMs ? sw1.heldMs : sw2.heldMs;
  SswOled_Text(fb, 1, 0, "CONTROL MODE", true);
  SswOled_Text(fb, 0, 1, "Keep holding 5 s", false);
  SswOled_Bar(fb, 0, 2, 128, h * 1000U / ENTER_HOLD_MS);
  SswOled_Text(fb, 0, 3, "Release to cancel", false);
}

static void drawControl(TickType_t now)
{
  char t[40];
  SswOled_Text(fb, 1, 0, "CONTROL MODE", true);
  uint32_t idle = (uint32_t)((now - lastAct) * portTICK_PERIOD_MS);
  uint32_t left = idle >= CONTROL_IDLE_MS ? 0U : (CONTROL_IDLE_MS - idle + 999U) / 1000U;
  (void)snprintf(t, sizeof t, "%2lus", (unsigned long)left);
  SswOled_Text(fb, SSW_OLED_W - 18U, 0, t, false);
  drawRelaysSel(1, sel < SEL_EXIT ? (int)sel : -1);
  bool on = sel < SEL_EXIT && ctl.effective[sel] == EFF_ON;
  if (sel == SEL_EXIT) SswOled_Text(fb, 0, 2, "> EXIT control mode", true);
  else {
    (void)snprintf(t, sizeof t, "CH%u is %s%s", sel + 1U, on ? "ON" : ctl.effective[sel] == EFF_OFF ? "OFF" : "?",
        ctl.cycle_remaining_s[sel] ? " (cycling)" : "");
    SswOled_Text(fb, 0, 2, t, false);
  }
  const char *verb = sel == SEL_EXIT ? "exit" : on ? "OFF" : "ON";
  if (armed && sw2.down && !sw2.used) {
    (void)snprintf(t, sizeof t, "%s", verb);
    SswOled_Text(fb, 0, 3, t, false);
    SswOled_Bar(fb, 30, 3, 98, sw2.heldMs * 1000U / ACT_HOLD_MS);
  } else if ((int32_t)(msgUntil - now) > 0 && msg[0]) SswOled_Text(fb, 0, 3, msg, false);
  else if (sel < SEL_EXIT && on && !ctl.coil_supply) SswOled_Text(fb, 0, 3, "OFF needs 24V", false);
  else { (void)snprintf(t, sizeof t, "SW1:next SW2 2s:%s", verb); SswOled_Text(fb, 0, 3, t, false); }
}

static void oledService(TickType_t now, bool input)
{
  if (!oledOk) {
    if ((int32_t)(now - oledRetryAt) < 0) return;
    if (!SswOled_Init(oledWrite, CONTRAST_FULL)) { oledFailed(); return; }
    oledOk = true; dim = false; lastDraw = 0;
    if (oledLogged) (void)DebugLog_Printf("UI: OLED ready\r\n");
    oledLogged = false;
  }
  bool holding = !controlMode && (sw1.heldMs >= HOLD_SHOW_MS || sw2.heldMs >= HOLD_SHOW_MS) && !sw1.used && !sw2.used;
  /* brightness: dim after 5 min without a button press, unless something needs attention */
  bool attention = controlMode || holding || level == LV_FAULT || level == LV_WARN || level == LV_UPDATE || level == LV_LOWPWR;
  bool wantDim = !attention && (now - lastInput) >= pdMS_TO_TICKS(DIM_AFTER_MS);
  if (wantDim != dim) {
    if (!SswOled_Contrast(oledWrite, wantDim ? CONTRAST_DIM : CONTRAST_FULL)) { oledFailed(); return; }
    dim = wantDim;
  }
  bool live = controlMode || holding || level == LV_UPDATE;
  bool turn = !live && (now - pageSince) >= pdMS_TO_TICKS(PAGE_MS);
  if (input && !wantDim && !live) turn = true;              /* short press steps to the next page */
  if (turn) { page = (page + 1U) % pageCount(); pageSince = now; }
  if (live || turn || input || (now - lastDraw) >= pdMS_TO_TICKS(REDRAW_MS)) {
    SswOled_Clear(fb);
    if (level == LV_UPDATE) drawUpdate();
    else if (controlMode) drawControl(now);
    else if (holding) drawHold();
    else render();
    if (!SswOled_Flush(oledWrite, fb, 0, SSW_OLED_PAGES - 1U)) {
      oledFailed();
      if (controlMode) leaveControl(now);                    /* no blind control without a display */
      return;
    }
    lastDraw = now;
  }
}

/* ---- task ----------------------------------------------------------------- */
static void uiTask(void *arg)
{
  (void)arg;
  /* S6-1: no OLED traffic before Control has done the first PCA9536 read. */
  for (unsigned i = 0; i < 50U; i++) {
    Control_GetSnapshot(&ctl);
    if (ctl.available) break;
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
  }
  TickType_t wake = xTaskGetTickCount(), lastEval = 0;
  lastInput = pageSince = wake;
  oledRetryAt = wake;
  unsigned slot = 0;
  evaluate();
  for (;;) {
    TickType_t now = xTaskGetTickCount();
    bool wasDim = dim;
    bool input = buttons(now);
    if (input || sw1.down || sw2.down) lastInput = now;
    if (input && wasDim) input = false;                     /* first press only wakes */
    if (controlMode) Control_GetSnapshot(&ctl);
    if (now - lastEval >= pdMS_TO_TICKS(500) || fw.reboot_pending) { evaluate(); lastEval = now; }

    uint32_t bits; unsigned len;
    ledPattern(level, &bits, &len);
    if (slot >= len) slot = 0;
    HAL_GPIO_WritePin(LED_Blue_GPIO_Port, LED_Blue_Pin, (bits >> slot) & 1U ? GPIO_PIN_SET : GPIO_PIN_RESET);
    slot++;

    oledService(now, input);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
  }
}

bool LocalUi_Start(void)
{
  if (task != NULL) return true;
  task = xTaskCreateStatic(uiTask, "ui", sizeof(stack) / sizeof(stack[0]), NULL,
      osPriorityLow, stack, &tcb);
  return task != NULL;
}
