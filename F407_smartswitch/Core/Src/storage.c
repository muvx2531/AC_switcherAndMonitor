#include "storage.h"
#include "w25q128.h"
#include "boot.h"
#include "debug_log.h"
#include "supervisor.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "cmsis_os2.h"
#include <stddef.h>
#include <string.h>

#define CHIP            W25Q128_CHIP_1
#define SECTOR          W25Q128_SECTOR_SIZE
#define CFG_A           0x000000UL
#define CFG_B           0x001000UL
#define SUPER           0x002000UL
#define ENERGY_BASE     0x010000UL
#define ENERGY_SIZE     0x010000UL
#define EVENT_BASE      0x020000UL
#define EVENT_SIZE      0x100000UL
#define HIST_BASE       0x120000UL
#define HIST_SIZE       0x400000UL
#define SUPER_MAGIC     0x54535353UL      /* "SSST" */
#define LAYOUT_VERSION  1U
#define CFG_MAGIC       0x47464353UL      /* "SCFG" */
#define BLANK32         0xFFFFFFFFUL
#define QUEUE_LEN       24U

/* ---- CRC-32 (IEEE, nibble table) ----------------------------------------- */
static uint32_t crc32(const void *data, size_t len)
{
  static const uint32_t t[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C };
  const uint8_t *p = data;
  uint32_t c = 0xFFFFFFFFU;
  while (len--) { c ^= *p++; c = (c >> 4) ^ t[c & 15U]; c = (c >> 4) ^ t[c & 15U]; }
  return ~c;
}

/* ---- on-flash records ----------------------------------------------------- */
typedef struct { uint32_t seq; uint32_t rtc_s; uint32_t uptime_s; uint16_t type; uint16_t rsv;
                 int32_t a, b, c; uint32_t crc; } EventRec;                     /* 32 B; d dropped */
typedef struct { uint32_t seq; uint32_t rtc_s; HistEntry e; uint16_t rsv; uint32_t crc; } HistRec;  /* 32 B */
typedef struct { uint32_t seq; uint32_t rsv; uint64_t uwh; uint32_t rsv2[3]; uint32_t crc; } EnergyRec; /* 32 B */
typedef struct { uint32_t magic; uint32_t seq; uint8_t amc; uint8_t rsv; int16_t ctph; uint32_t rsv2[3]; uint32_t crc; } CfgRec; /* 28 B */
typedef struct { uint32_t magic; uint32_t version; uint32_t rsv[5]; uint32_t crc; } SuperRec;         /* 32 B */
/* Config v2 (remote link settings). v1 records (CFG_MAGIC) are still read; the next write is v2. */
typedef struct { uint32_t magic; uint32_t seq; uint8_t amc; uint8_t rsv; int16_t ctph;
                 uint8_t r_en, r_first, r_last, r_gap, r_max, r_int; uint16_t r_port; uint32_t r_fixed;
                 char r_key[68]; uint32_t rsv2[2]; uint32_t crc; } CfgRec2;                      /* 104 B */
#define CFG2_MAGIC      0x32464353UL      /* "SCF2" */
_Static_assert(sizeof(CfgRec2) == 104, "CfgRec2 size");

_Static_assert(sizeof(EventRec) == 32, "EventRec size");
_Static_assert(sizeof(HistRec) == 32, "HistRec size");
_Static_assert(sizeof(SuperRec) == 32, "SuperRec size");
_Static_assert(sizeof(EnergyRec) == 32, "EnergyRec size");

/* ---- generic ring of fixed-size records ----------------------------------- */
typedef struct {
  uint32_t base, size, rec;      /* rec divides 256 */
  uint32_t pos;                  /* next write address */
  uint32_t seq;                  /* last written sequence (0 = none) */
  uint32_t count;                /* valid records (approximate after wrap: full sectors) */
} Ring;

static Ring evRing = { EVENT_BASE, EVENT_SIZE, sizeof(EventRec), 0, 0, 0 };
static Ring hiRing = { HIST_BASE, HIST_SIZE, sizeof(HistRec), 0, 0, 0 };
static Ring enRing = { ENERGY_BASE, ENERGY_SIZE, sizeof(EnergyRec), 0, 0, 0 };

static uint8_t rbuf[SECTOR] __attribute__((section(".ccmbss")));   /* storage task / init only */
static uint8_t wbuf[256];                                            /* program source (SRAM) */

static bool recValid(const uint8_t *r, uint32_t rec)
{
  uint32_t seq, crc;
  memcpy(&seq, r, 4);
  if (seq == BLANK32 || seq == 0U) return false;
  uint32_t crcOff = rec - 4U;                    /* every record: CRC-32 in the last word */
  memcpy(&crc, r + crcOff, 4);
  return crc32(r, crcOff) == crc;
}

static uint32_t recSeq(const uint8_t *r) { uint32_t s; memcpy(&s, r, 4); return s; }

/* Mount: find the newest record by reading the first record of every sector, then scan
 * the newest sector. */
static bool ringMount(Ring *g)
{
  uint32_t bestSeq = 0, bestSec = BLANK32, sectors = g->size / SECTOR, validSectors = 0;
  uint8_t r[32];
  for (uint32_t s = 0; s < sectors; s++) {
    if (W25Q128_Read(CHIP, g->base + s * SECTOR, r, g->rec) != W25Q128_OK) return false;
    if (recValid(r, g->rec)) {
      validSectors++;
      if (recSeq(r) > bestSeq) { bestSeq = recSeq(r); bestSec = s; }
    }
    if ((s & 63U) == 0U) Supervisor_Heartbeat();
  }
  if (bestSec == BLANK32) { g->pos = g->base; g->seq = 0; g->count = 0; return true; }
  uint32_t secAddr = g->base + bestSec * SECTOR;
  if (W25Q128_Read(CHIP, secAddr, rbuf, SECTOR) != W25Q128_OK) return false;
  uint32_t perSec = SECTOR / g->rec, k = 0;
  while (k < perSec && recValid(&rbuf[k * g->rec], g->rec) && recSeq(&rbuf[k * g->rec]) >= bestSeq) {
    g->seq = recSeq(&rbuf[k * g->rec]); k++;
  }
  g->pos = secAddr + k * g->rec;
  if (g->pos >= g->base + g->size) g->pos = g->base;
  g->count = (validSectors - 1U) * perSec + k;
  return true;
}

static bool ringAppend(Ring *g, const void *data)
{
  if ((g->pos % SECTOR) == 0U) {
    if (W25Q128_EraseSector(CHIP, g->pos) != W25Q128_OK) return false;
    uint32_t perSec = SECTOR / g->rec;
    if (g->count >= g->size / g->rec - perSec) g->count -= perSec;   /* oldest sector reused */
  }
  memcpy(wbuf, data, g->rec);
  if (W25Q128_ProgramPage(CHIP, g->pos, wbuf, g->rec) != W25Q128_OK) return false;
  uint8_t v[32];
  if (W25Q128_Read(CHIP, g->pos, v, g->rec) != W25Q128_OK || memcmp(v, data, g->rec) != 0) return false;
  memcpy(&g->seq, data, 4);
  g->pos += g->rec;
  if (g->pos >= g->base + g->size) g->pos = g->base;
  g->count++;
  return true;
}

/* ---- state ---------------------------------------------------------------- */
typedef enum { Q_EVENT, Q_MINUTE, Q_ENERGY, Q_CONFIG, Q_FORMAT } QKind;
typedef struct {
  uint8_t kind;
  union {
    EventRec ev;
    HistRec hi;
    uint64_t uwh;
    StConfig cfg;
  } u;
} QItem;

static QueueHandle_t queue;
static StaticQueue_t queueStorage;
static uint8_t queueBuf[QUEUE_LEN * sizeof(QItem)] __attribute__((section(".ccmbss")));
static SemaphoreHandle_t lock;          /* ring metadata + flash readers vs writer */
static StaticSemaphore_t lockStorage;
static TaskHandle_t task;
static StaticTask_t tcb;
static StackType_t stack[512] __attribute__((section(".ccmram")));

static volatile StState state = ST_STATE_INIT;
static StConfig config = { .seq = 0, .amc_fitted = 0xFF, .ct_phase_cdeg = 0,
  .remote = { .enabled = 1, .ip_first = 10, .ip_last = 20, .min_gap_s = 5, .max_per_min = 10, .interval_s = 1, .port = 9001 } };
static uint32_t cfgSlotNext = CFG_A;
static uint64_t restoredUwh, savedUwh;
static volatile uint32_t dropped, writeErrors;

const char *Storage_StateName(StState s)
{
  switch (s) {
    case ST_STATE_READY: return "READY";
    case ST_STATE_UNFORMATTED: return "UNFORMATTED";
    case ST_STATE_FAULT: return "FAULT";
    default: return "INIT";
  }
}

void Storage_RemoteDefaults(StRemoteConfig *r)
{
  memset(r, 0, sizeof(*r));
  r->enabled = 1U; r->ip_first = 10U; r->ip_last = 20U; r->port = 9001U;
  r->interval_s = 1U; r->min_gap_s = 5U; r->max_per_min = 10U;
}

static void setDefaults(StConfig *c)
{
  memset(c, 0, sizeof(*c));
  c->amc_fitted = 0xFFU;
  Storage_RemoteDefaults(&c->remote);
}

/* Reads a v2 or v1 config record into *out (v1: remote defaults). */
static bool readCfg(uint32_t addr, StConfig *out)
{
  CfgRec2 c;
  if (W25Q128_Read(CHIP, addr, &c, sizeof(c)) != W25Q128_OK || c.seq == BLANK32) return false;
  setDefaults(out);
  if (c.magic == CFG2_MAGIC && crc32(&c, offsetof(CfgRec2, crc)) == c.crc) {
    out->seq = c.seq; out->amc_fitted = c.amc; out->ct_phase_cdeg = c.ctph;
    StRemoteConfig *r = &out->remote;
    r->enabled = c.r_en; r->ip_first = c.r_first; r->ip_last = c.r_last; r->min_gap_s = c.r_gap;
    r->max_per_min = c.r_max; r->interval_s = c.r_int; r->port = c.r_port; r->fixed_ip = c.r_fixed;
    memcpy(r->key, c.r_key, ST_REMOTE_KEY_MAX);
    r->key[ST_REMOTE_KEY_MAX] = '\0';
    return true;
  }
  const CfgRec *v1 = (const CfgRec *)(const void *)&c;
  if (v1->magic == CFG_MAGIC && crc32(v1, offsetof(CfgRec, crc)) == v1->crc) {
    out->seq = v1->seq; out->amc_fitted = v1->amc; out->ct_phase_cdeg = v1->ctph;
    return true;
  }
  return false;
}

static void loadConfig(void)
{
  static StConfig a, b;            /* static: ~100 B each, called once from the default task */
  bool va = readCfg(CFG_A, &a), vb = readCfg(CFG_B, &b);
  const StConfig *best = NULL;
  if (va && (!vb || a.seq > b.seq)) { best = &a; cfgSlotNext = CFG_B; }
  else if (vb) { best = &b; cfgSlotNext = CFG_A; }
  if (best) { taskENTER_CRITICAL(); config = *best; taskEXIT_CRITICAL(); }
}

static bool writeConfig(const StConfig *in)
{
  static CfgRec2 c;
  static StConfig v;
  memset(&c, 0, sizeof(c));
  c.magic = CFG2_MAGIC; c.seq = config.seq + 1U; c.amc = in->amc_fitted; c.ctph = in->ct_phase_cdeg;
  const StRemoteConfig *r = &in->remote;
  c.r_en = r->enabled; c.r_first = r->ip_first; c.r_last = r->ip_last; c.r_gap = r->min_gap_s;
  c.r_max = r->max_per_min; c.r_int = r->interval_s; c.r_port = r->port; c.r_fixed = r->fixed_ip;
  memcpy(c.r_key, r->key, ST_REMOTE_KEY_MAX);
  c.crc = crc32(&c, offsetof(CfgRec2, crc));
  if (W25Q128_EraseSector(CHIP, cfgSlotNext) != W25Q128_OK) return false;
  memcpy(wbuf, &c, sizeof(c));
  if (W25Q128_ProgramPage(CHIP, cfgSlotNext, wbuf, sizeof(c)) != W25Q128_OK) return false;
  if (!readCfg(cfgSlotNext, &v) || v.seq != c.seq) return false;
  taskENTER_CRITICAL(); config = v; taskEXIT_CRITICAL();
  cfgSlotNext = cfgSlotNext == CFG_A ? CFG_B : CFG_A;
  return true;
}

static void loadEnergy(void)
{
  if (enRing.seq == 0U) return;
  uint32_t last = enRing.pos == enRing.base ? enRing.base + enRing.size - enRing.rec : enRing.pos - enRing.rec;
  uint8_t r[32];
  if (W25Q128_Read(CHIP, last, r, enRing.rec) == W25Q128_OK && recValid(r, enRing.rec)) {
    EnergyRec e; memcpy(&e, r, sizeof(e));
    restoredUwh = savedUwh = e.uwh;
  }
}

/* Superblock present -> READY. Absent and every partition sector starts blank -> write it.
 * Otherwise foreign data: stay UNFORMATTED (never erase what we did not write). */
static bool checkFormat(bool forceFormat)
{
  SuperRec s;
  if (W25Q128_Read(CHIP, SUPER, &s, sizeof(s)) != W25Q128_OK) return false;
  if (!forceFormat && s.magic == SUPER_MAGIC && s.version == LAYOUT_VERSION && crc32(&s, offsetof(SuperRec, crc)) == s.crc)
    return true;
  if (!forceFormat) {
    static const uint32_t regions[][2] = { { CFG_A, 3U * SECTOR }, { ENERGY_BASE, ENERGY_SIZE }, { EVENT_BASE, EVENT_SIZE }, { HIST_BASE, HIST_SIZE } };
    uint8_t r[32];
    for (unsigned int i = 0; i < 4U; i++) {
      for (uint32_t a = regions[i][0]; a < regions[i][0] + regions[i][1]; a += SECTOR) {
        if (W25Q128_Read(CHIP, a, r, sizeof(r)) != W25Q128_OK) return false;
        for (unsigned int k = 0; k < sizeof(r); k++) if (r[k] != 0xFFU) {
          (void)DebugLog_Printf("STORAGE: foreign data at 0x%06lX, writes disabled (serial STFORMAT erases)\r\n", (unsigned long)a);
          state = ST_STATE_UNFORMATTED;
          return false;
        }
        if (((a / SECTOR) & 63U) == 0U) Supervisor_Heartbeat();
      }
    }
  } else {
    (void)DebugLog_Printf("STORAGE: formatting chip 1 partitions\r\n");
    static const uint32_t regions[][2] = { { CFG_A, 3U * SECTOR }, { ENERGY_BASE, ENERGY_SIZE }, { EVENT_BASE, EVENT_SIZE }, { HIST_BASE, HIST_SIZE } };
    for (unsigned int i = 0; i < 4U; i++)
      for (uint32_t a = regions[i][0]; a < regions[i][0] + regions[i][1]; a += SECTOR) {
        if (W25Q128_EraseSector(CHIP, a) != W25Q128_OK) return false;
        Supervisor_Heartbeat();
      }
  }
  memset(&s, 0xFF, sizeof(s));
  s.magic = SUPER_MAGIC; s.version = LAYOUT_VERSION;
  s.crc = crc32(&s, offsetof(SuperRec, crc));
  if (W25Q128_EraseSector(CHIP, SUPER) != W25Q128_OK) return false;
  memcpy(wbuf, &s, sizeof(s));
  if (W25Q128_ProgramPage(CHIP, SUPER, wbuf, sizeof(s)) != W25Q128_OK) return false;
  (void)DebugLog_Printf("STORAGE: superblock written (layout %u)\r\n", LAYOUT_VERSION);
  return true;
}

static bool mountAll(void)
{
  return ringMount(&evRing) && ringMount(&hiRing) && ringMount(&enRing);
}

bool Storage_Init(void)
{
  lock = xSemaphoreCreateMutexStatic(&lockStorage);
  queue = xQueueCreateStatic(QUEUE_LEN, sizeof(QItem), queueBuf, &queueStorage);
  if (lock == NULL || queue == NULL) { state = ST_STATE_FAULT; return false; }
  uint8_t id[3];
  if (W25Q128_ReadId(CHIP, id) != W25Q128_OK || id[0] != 0xEFU) { state = ST_STATE_FAULT; return false; }
  if (!checkFormat(false)) {
    if (state != ST_STATE_UNFORMATTED) state = ST_STATE_FAULT;
    return false;
  }
  loadConfig();
  if (!mountAll()) { state = ST_STATE_FAULT; return false; }
  loadEnergy();
  state = ST_STATE_READY;
  (void)DebugLog_Printf("STORAGE: ready, events %lu (seq %lu), history %lu, energy %lu mWh, config seq %lu\r\n",
      (unsigned long)evRing.count, (unsigned long)evRing.seq, (unsigned long)hiRing.count,
      (unsigned long)(restoredUwh / 1000U), (unsigned long)config.seq);
  return true;
}

static void handle(QItem *q)
{
  bool ok = true;
  xSemaphoreTake(lock, portMAX_DELAY);
  switch (q->kind) {
    case Q_EVENT:
      q->u.ev.seq = evRing.seq + 1U; q->u.ev.crc = crc32(&q->u.ev, offsetof(EventRec, crc));
      ok = ringAppend(&evRing, &q->u.ev); break;
    case Q_MINUTE:
      q->u.hi.seq = hiRing.seq + 1U; q->u.hi.crc = crc32(&q->u.hi, offsetof(HistRec, crc));
      ok = ringAppend(&hiRing, &q->u.hi); break;
    case Q_ENERGY: {
      EnergyRec e; memset(&e, 0xFF, sizeof(e));
      e.seq = enRing.seq + 1U; e.rsv = 0; e.uwh = q->u.uwh;
      e.crc = crc32(&e, offsetof(EnergyRec, crc));
      ok = ringAppend(&enRing, &e);
      if (ok) savedUwh = q->u.uwh;
      break;
    }
    case Q_CONFIG:
      ok = writeConfig(&q->u.cfg); break;
    case Q_FORMAT:
      ok = checkFormat(true) && mountAll();
      if (ok) { taskENTER_CRITICAL(); setDefaults(&config); taskEXIT_CRITICAL(); cfgSlotNext = CFG_A; restoredUwh = savedUwh = 0; state = ST_STATE_READY; }
      break;
  }
  xSemaphoreGive(lock);
  if (!ok) {
    writeErrors++;
    (void)DebugLog_Printf("STORAGE: write failed (kind %u)\r\n", q->kind);
    if (writeErrors >= 5U) state = ST_STATE_FAULT;
  }
}

static void storageTask(void *arg)
{
  (void)arg;
  QItem q;
  for (;;) {
    if (xQueueReceive(queue, &q, pdMS_TO_TICKS(500)) == pdTRUE) {
      if (state == ST_STATE_READY || (q.kind == Q_FORMAT && state != ST_STATE_FAULT)) handle(&q);
    }
    Supervisor_TaskHeartbeat(SUP_TASK_STORAGE);
  }
}

bool Storage_Start(void)
{
  if (task != NULL || queue == NULL) return task != NULL;
  /* Below Supervisor: a slow erase must never starve the watchdog kick. */
  task = xTaskCreateStatic(storageTask, "storage", sizeof(stack) / sizeof(stack[0]), NULL,
      osPriorityLow, stack, &tcb);
  return task != NULL;
}

static void post(QItem *q)
{
  if (queue == NULL || (state != ST_STATE_READY && q->kind != Q_FORMAT)) return;
  if (xQueueSend(queue, q, 0) != pdTRUE) dropped++;
}

void Storage_LogEvent(StEventType type, int32_t a, int32_t b, int32_t c, int32_t d)
{
  (void)d;
  QItem q; memset(&q, 0xFF, sizeof(q));
  q.kind = Q_EVENT;
  q.u.ev.rtc_s = Boot_RtcKept() ? Boot_RtcSeconds() : 0U;
  q.u.ev.uptime_s = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
  q.u.ev.type = (uint16_t)type; q.u.ev.rsv = 0; q.u.ev.a = a; q.u.ev.b = b; q.u.ev.c = c;
  post(&q);
}

void Storage_SaveMinute(const HistEntry *e)
{
  QItem q; memset(&q, 0xFF, sizeof(q));
  q.kind = Q_MINUTE; q.u.hi.rtc_s = Boot_RtcSeconds(); q.u.hi.e = *e; q.u.hi.rsv = 0;
  post(&q);
}

void Storage_CheckpointEnergy(uint64_t energy_uwh)
{
  if (energy_uwh == savedUwh) return;
  QItem q; q.kind = Q_ENERGY; q.u.uwh = energy_uwh;
  post(&q);
}

bool Storage_SetConfig(const StConfig *cfg)
{
  if (state != ST_STATE_READY) return false;
  QItem q; q.kind = Q_CONFIG; q.u.cfg = *cfg;
  if (queue == NULL || xQueueSend(queue, &q, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  return true;
}

void Storage_RequestFormat(void)
{
  if (queue == NULL || state == ST_STATE_FAULT) return;
  QItem q; q.kind = Q_FORMAT;
  (void)xQueueSend(queue, &q, pdMS_TO_TICKS(100));
}

void Storage_GetConfig(StConfig *out) { taskENTER_CRITICAL(); *out = config; taskEXIT_CRITICAL(); }
uint64_t Storage_RestoredEnergy(void) { return restoredUwh; }

void Storage_GetStatus(StStatus *out)
{
  taskENTER_CRITICAL();
  out->state = state; out->event_seq = evRing.seq; out->history_seq = hiRing.seq; out->energy_seq = enRing.seq;
  out->history_records = hiRing.count; out->dropped = dropped; out->write_errors = writeErrors;
  out->energy_uwh_saved = savedUwh;
  taskEXIT_CRITICAL();
}

uint32_t Storage_LastEventSeq(void) { return state == ST_STATE_READY ? evRing.seq : 0U; }

/* ---- readers -------------------------------------------------------------- */
static uint8_t readerBuf[SECTOR] __attribute__((section(".ccmbss")));   /* web readers, under lock */

uint32_t Storage_ReadEvents(StEvent *out, uint32_t max) { return Storage_ReadEventsBefore(UINT32_MAX, out, max); }

uint32_t Storage_ReadEventsBefore(uint32_t before_seq, StEvent *out, uint32_t max)
{
  if (state != ST_STATE_READY || max == 0U || before_seq == 0U) return 0U;
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t n = 0, addr = evRing.pos, expect = evRing.seq;
  if (before_seq <= expect) {
    /* skip the newer records: step back (expect - before_seq + 1) slots in the ring */
    uint32_t skip = expect - before_seq + 1U, slots = evRing.size / evRing.rec;
    if (skip >= slots) skip = slots;               /* older than the ring: nothing valid there */
    uint32_t off = (addr - evRing.base) / evRing.rec;
    off = (off + slots - (skip % slots)) % slots;
    addr = evRing.base + off * evRing.rec;
    expect = skip >= slots ? 0U : before_seq - 1U;
  }
  while (n < max && expect > 0U) {
    addr = addr == evRing.base ? evRing.base + evRing.size - evRing.rec : addr - evRing.rec;
    EventRec r;
    if (W25Q128_Read(CHIP, addr, &r, sizeof(r)) != W25Q128_OK || !recValid((uint8_t *)&r, sizeof(r)) || r.seq != expect) break;
    out[n].seq = r.seq; out[n].rtc_s = r.rtc_s; out[n].uptime_s = r.uptime_s; out[n].type = r.type;
    out[n].a = r.a; out[n].b = r.b; out[n].c = r.c; out[n].d = 0;
    n++; expect--;
  }
  xSemaphoreGive(lock);
  return n;
}

uint32_t Storage_ReadHistory(uint32_t from_rtc_s, StHistoryFn fn, void *ctx)
{
  if (state != ST_STATE_READY || hiRing.seq == 0U) return 0U;
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t sectors = hiRing.size / SECTOR, perSec = SECTOR / hiRing.rec;
  uint32_t head = (hiRing.pos - hiRing.base) / SECTOR;          /* sector being written */
  if ((hiRing.pos - hiRing.base) % SECTOR == 0U) head = (head + sectors - 1U) % sectors;
  /* walk back to the first sector that starts before from_rtc_s (or the oldest) */
  uint32_t start = head, steps = 0;
  HistRec r;
  while (steps < sectors - 1U) {
    uint32_t prev = (start + sectors - 1U) % sectors;
    if (W25Q128_Read(CHIP, hiRing.base + start * SECTOR, &r, sizeof(r)) != W25Q128_OK) break;
    if (!recValid((uint8_t *)&r, sizeof(r)) || r.rtc_s < from_rtc_s) break;
    HistRec pr;
    if (W25Q128_Read(CHIP, hiRing.base + prev * SECTOR, &pr, sizeof(pr)) != W25Q128_OK ||
        !recValid((uint8_t *)&pr, sizeof(pr)) || pr.seq >= r.seq) break;
    start = prev; steps++;
  }
  uint32_t n = 0, lastSeq = 0;
  for (uint32_t s = start, k = 0; k <= steps; k++, s = (s + 1U) % sectors) {
    if (W25Q128_Read(CHIP, hiRing.base + s * SECTOR, readerBuf, SECTOR) != W25Q128_OK) break;
    for (uint32_t i = 0; i < perSec; i++) {
      HistRec *h = (HistRec *)&readerBuf[i * hiRing.rec];
      if (!recValid((uint8_t *)h, hiRing.rec) || h->seq <= lastSeq) break;
      lastSeq = h->seq;
      if (h->rtc_s >= from_rtc_s) { fn(h->rtc_s, &h->e, ctx); n++; }
    }
  }
  xSemaphoreGive(lock);
  return n;
}
