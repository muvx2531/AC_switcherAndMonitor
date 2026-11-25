#ifndef CONTROL_H
#define CONTROL_H
#include <stdbool.h>
#include <stdint.h>

/* Control service: sole owner of the relay state and the only writer of the
 * PCA9536 after boot. See docs/architecture/tasks/control.md. Invariants:
 * I1 read the expander first, I2 never change coil state on its own except
 * (a) user command, (b) finishing a timed command (also after reset), (c) latch
 * LOW on all channels when the 24 V coil supply is gone; I3 no write while the
 * state is unknown; I4 never restore OFF from storage after expander power-on. */

#define CONTROL_CHANNELS 4U
#define CONTROL_ALL_CHANNELS 0x0FU
#define CONTROL_DEFAULT_CYCLE_S 10U
#define CONTROL_MAX_CYCLE_S 600U

typedef enum { CTRL_SET_ON, CTRL_SET_OFF, CTRL_POWER_CYCLE, CTRL_CANCEL_CYCLE } CtrlType;
typedef enum { SRC_WEB, SRC_SERIAL, SRC_LOCAL, SRC_SYSTEM } CtrlSource;

typedef struct {
  CtrlType type;
  uint8_t channel_mask;   /* bit0..3 = CH1..4 */
  uint16_t duration_s;    /* POWER_CYCLE: 1..600, 0 = default 10 s */
  CtrlSource source;
} ControlRequest;

typedef enum {
  CTRL_SUBMIT_ACCEPTED,
  CTRL_SUBMIT_BUSY,              /* queue full */
  CTRL_SUBMIT_REJECTED_INVALID,
  CTRL_SUBMIT_REJECTED_NO_COIL_SUPPLY,
  CTRL_SUBMIT_REJECTED_STATE_UNKNOWN,
  CTRL_SUBMIT_NOT_READY,
  CTRL_SUBMIT_REJECTED_LOCAL_CONTROL,   /* button control mode active: web/serial refused (D1) */
} CtrlSubmitResult;

typedef enum { OUT_PENDING, OUT_APPLIED, OUT_FAILED, OUT_SUPERSEDED, OUT_UNKNOWN, OUT_REJECTED } CtrlOutcomeState;

typedef struct {
  uint32_t request_id;
  CtrlOutcomeState state;
  const char *reason;     /* static string or NULL */
} CtrlOutcome;

typedef enum { EFF_ON, EFF_OFF, EFF_UNKNOWN } CtrlEffective;

typedef struct {
  bool available;          /* Control task running */
  bool state_known;        /* last PCA read-back succeeded */
  bool coil_supply;        /* confirmed 24 V coil supply (PB3) */
  uint8_t latch;           /* PCA output latch, low nibble */
  uint8_t config;          /* PCA configuration, low nibble (1 = input) */
  CtrlEffective effective[CONTROL_CHANNELS];
  uint16_t cycle_remaining_s[CONTROL_CHANNELS];
  CtrlSource last_source[CONTROL_CHANNELS];
  uint32_t glitch_count;   /* PB3 edges rejected by the confirmation filter */
  uint32_t supply_releases;/* times rule (c) released latches */
  const char *boot_note;   /* result of the pending_restore check at boot */
  bool local_lock;         /* button control mode active: web/serial commands refused */
} ControlSnapshot;

/* Start once from task context after I2C init and the PCA9536 boot read. */
bool Control_Start(void);
/* Task context. On ACCEPTED, *out_request_id identifies the outcome. */
CtrlSubmitResult Control_Submit(const ControlRequest *req, uint32_t *out_request_id);
/* false if the id is unknown or has dropped out of the outcome history. */
bool Control_GetOutcome(uint32_t request_id, CtrlOutcome *out);
void Control_GetSnapshot(ControlSnapshot *out);
/* Local UI button control mode (decision D1, 2026-11-25: buttons have authority). While
 * the lock is held, Control_Submit refuses SRC_WEB and SRC_SERIAL requests. The holder
 * refreshes it at least every few seconds; it expires by itself 15 s after the last
 * refresh, so a stuck UI never locks out the web for good. */
void Control_LocalLock(bool on);
const char *Control_OutcomeName(CtrlOutcomeState state);
const char *Control_SubmitResultName(CtrlSubmitResult result);

#endif
