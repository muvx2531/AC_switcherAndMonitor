#ifndef STORAGE_H
#define STORAGE_H
#include <stdbool.h>
#include <stdint.h>
#include "pq_history.h"

/* Storage service (docs/architecture/tasks/storage.md, Phase 5a): the only writer of the
 * W25Q128 flash. Chip 1 layout (decision D8, 2026-11-25):
 *
 *   0x000000  4 KiB   config A        A/B copies, highest valid sequence wins
 *   0x001000  4 KiB   config B
 *   0x002000  4 KiB   superblock      layout magic/version; written once at format
 *   0x010000  64 KiB  energy ring     16-byte checkpoints of the energy counter
 *   0x020000  1 MiB   event log       32-byte records, ring of 4 KiB sectors
 *   0x120000  4 MiB   PQ history      32-byte minute records (~91 days)
 *   0xFFF000  4 KiB   reserved for the bring-up flash test
 * Chip 2 is untouched (firmware slots come with the bootloader, Phase 5b).
 *
 * Every record carries a sequence number and a CRC-32; a sector is erased just before
 * it is reused. Program operations are read back. Writes happen only in the storage
 * task; other tasks queue requests (non-blocking) and read through Storage_Read*.
 * Relay state is never restored from flash (I4). */

typedef enum {
  ST_EV_BOOT = 1,        /* a: reset cause index, b: reset count, c: safe mode */
  ST_EV_RELAY,           /* a: request id, b: type, c: channel mask, d: outcome<<8 | source */
  ST_EV_RULE_C,          /* a: released mask */
  ST_EV_SUPPLY,          /* a: 1 = 24 V present, 0 = lost */
  ST_EV_POWER_MODE,      /* a: from, b: to */
  ST_EV_PQ,              /* a: type (AcqPqEvent), b: duration ms, c: extreme V x10 */
  ST_EV_CONTACT,         /* a: channel, b: AcqPqContact */
  ST_EV_CT_REVERSED,     /* a: 1 set, 0 cleared */
  ST_EV_CONFIG,          /* a: config sequence */
  ST_EV_FORMAT,          /* storage formatted */
  ST_EV_FIRMWARE,        /* a: boot state (SswBootState), b: result (SswResult), c: version */
} StEventType;

typedef struct {
  uint32_t seq;
  uint32_t rtc_s;        /* RTC seconds since 2000-01-01 (0: RTC not trusted) */
  uint32_t uptime_s;
  uint16_t type;
  int32_t a, b, c, d;
} StEvent;

typedef struct {
  uint32_t seq;          /* config generation, set by Storage */
  uint8_t amc_fitted;    /* 0xFF = follow build default (ACQ_AMC_FITTED) */
  int16_t ct_phase_cdeg; /* CT phase lead, 0.01 degree */
} StConfig;

typedef enum {
  ST_STATE_INIT = 0,
  ST_STATE_READY,
  ST_STATE_UNFORMATTED,  /* foreign data found: writes disabled until serial STFORMAT */
  ST_STATE_FAULT,        /* flash not answering or repeated write failures */
} StState;

typedef struct {
  StState state;
  uint32_t event_seq, history_seq, energy_seq;
  uint32_t history_records;      /* valid minute records on flash */
  uint32_t dropped;              /* requests dropped because the queue was full */
  uint32_t write_errors;
  uint64_t energy_uwh_saved;
} StStatus;

/* Default task, after W25Q128_Init and before Acquisition_Start: mounts chip 1, loads the
 * config and the last energy checkpoint, finds the log heads. May take ~1 s. */
bool Storage_Init(void);
bool Storage_Start(void);

/* Any task (not ISR). Non-blocking; dropped and counted when the queue is full. */
void Storage_LogEvent(StEventType type, int32_t a, int32_t b, int32_t c, int32_t d);
void Storage_SaveMinute(const HistEntry *e);
void Storage_CheckpointEnergy(uint64_t energy_uwh);
bool Storage_SetConfig(const StConfig *cfg);
/* Erase the Storage partitions of chip 1 and format (serial STFORMAT only). */
void Storage_RequestFormat(void);

void Storage_GetConfig(StConfig *out);
uint64_t Storage_RestoredEnergy(void);
void Storage_GetStatus(StStatus *out);
const char *Storage_StateName(StState s);

/* Readers (web, task context). Newest first, up to max; returns the count. */
uint32_t Storage_ReadEvents(StEvent *out, uint32_t max);
/* Iterate history records with rtc_s >= from_rtc_s, oldest first. */
typedef void (*StHistoryFn)(uint32_t rtc_s, const HistEntry *e, void *ctx);
uint32_t Storage_ReadHistory(uint32_t from_rtc_s, StHistoryFn fn, void *ctx);

#endif
