# USART6 DMA debug logging: agent handoff

## Purpose and files

This module sends diagnostics over USART6 using a 10,240-byte static ring
buffer and DMA. It is a bring-up logging facility, not an audit log: messages
can be dropped under contention or load.

- Interface: `F407_smartswitch/Core/Inc/debug_log.h`
- Implementation: `F407_smartswitch/Core/Src/debug_log.c`
- Integration and heartbeat: `F407_smartswitch/Core/Src/main.c`, in CubeMX
  `USER CODE` sections. The default task initializes the module and sends a
  heartbeat every 500 ms while toggling `LED_Blue`.
- PC receiver: `tools/serial-monitor.ps1`.

## Board and peripheral configuration

USART6 uses PC6 TX and PC7 RX, 115200 baud, 8 data bits, no parity, one stop
bit, no flow control. ST-LINK/V3 virtual COM was observed as COM3; discover
the port again on another machine or after reconnecting. TX is DMA2 Stream 6,
channel 5, normal mode. DMA2 Stream 6 and USART6 interrupts are enabled at
priority 5; FreeRTOS `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` is 5.
Both IRQ paths are needed for HAL's normal-mode UART DMA completion. Verify
interrupt priorities again if CubeMX or FreeRTOS settings change.

The board crystal is **16 MHz**. The current `.ioc` and generated
`stm32f4xx_hal_conf.h` specify `HSE_VALUE=16000000`, with PLLM=16,
PLLN=336 and PLLP=2, yielding SYSCLK 168 MHz. Do not revert HSE to 25 MHz:
that mismatch caused incorrect UART timing on the actual board.

## API and ownership

Initialize once from a task after the scheduler starts and USART6/DMA are
initialized. `main.c` currently calls:

```c
if (!DebugLog_Init(&huart6)) {
    /* Record or handle initialization failure without relying on this logger. */
}
```

Other task-context callers can use:

```c
#include "debug_log.h"

if (!DebugLog_Printf("PHY link: %s\r\n", link_up ? "up" : "down")) {
    /* Logging is best effort; choose whether to account for this failure. */
}

const char message[] = "PHY probe started\r\n";
(void)DebugLog_Write(message, sizeof(message) - 1U);

DebugLogStats stats;
if (DebugLog_GetStats(&stats)) {
    /* Inspect stats; do not assume the snapshot call always succeeds. */
}
```

`DebugLog_Printf` prepends `[<tick-derived milliseconds> ms] ` and formats
into a 256-byte local stack array. The formatted message, timestamp included,
must fit including its terminating NUL. It does not add a newline; callers
should supply `\r\n` for line-oriented output. `DebugLog_Write` copies 1–256
bytes exactly as provided, adds no timestamp or newline, and the caller may
reuse its buffer after the function returns. Both return whether data was
queued, not whether it was received or physically transmitted. The timestamp
is uptime-derived, not wall-clock time, and wraps with the 32-bit tick/time.

All public calls are task-context only; none is safe from ISR. Initialize once
before other tasks start logging; concurrent calls to `DebugLog_Init` are not
supported. Do not independently send on USART6 with HAL polling, interrupt,
or DMA while the logger owns it.

### UART callback ownership (register callbacks)

`USE_HAL_UART_REGISTER_CALLBACKS` is enabled (`stm32f4xx_hal_conf.h`, and
`ProjectManager.RegisterCallBack=UART` in the `.ioc` so CubeMX keeps it). No
module may define the global `HAL_UART_*Callback` functions; they remain HAL's
weak defaults. Each module registers a static callback on the handle it uses
with `HAL_UART_RegisterCallback`:

| Handle / callback ID | Owner |
|---|---|
| USART6 `HAL_UART_TX_COMPLETE_CB_ID` | `debug_log.c` (`DebugLog_Init`) |
| USART6 `HAL_UART_RX_COMPLETE_CB_ID`, `HAL_UART_ERROR_CB_ID` | `debug_protocol.c` (`DebugProtocol_Start`) |

HAL accepts registration only while the handle's TX state (`gState`) is
READY. Because USART6 is shared, `StartDefaultTask` calls `DebugLog_Init`
and `DebugProtocol_Start` before the first log message is queued; both return
false if registration fails. USART6 has a single error callback for both
directions, so a TX DMA error also marks the protocol receiver failed (same
behavior as before the change). New UART drivers (USART1 modem, USART2 RS485)
register on their own handles in their init functions. Built in Debug and
Release on 2026-11-23; not yet re-tested on hardware after this change.

## Concurrency and failure behavior

Producers take a statically allocated FreeRTOS mutex with zero wait, copy a
complete message into the ring, and notify the single `debugTx` worker. A
producer does not wait for UART transmission or available buffer capacity;
mutex contention can drop a message even when there is free space. Formatting
and memory copies still consume CPU and stack, so logging is not free or
hard-real-time. The worker uses a 256-word dynamically allocated task stack and
priority `tskIDLE_PRIORITY + 2`; it takes the mutex while managing DMA/ring
state. The DMA completion ISR sets a flag and notifies the worker; only the
worker advances the ring tail. DMA accesses only the contiguous region before
the end of the ring, and the worker starts the next region after completion.

`DebugLog_GetStats` is a non-blocking snapshot: `queued_bytes` includes bytes
currently in flight, `high_water_bytes` is the maximum queued, and
`dropped_messages` includes mutex contention, insufficient buffer space and
formatted-message overflow. Invalid argument, ISR or uninitialized calls are
rejected but do not necessarily increment that counter. `dma_start_errors`
counts failed start attempts; the worker retries on its periodic wake.
`dma_transfer_errors` counts an in-flight transfer observed back in HAL READY
without a completion callback; such a chunk is discarded, not retransmitted.
There is currently no timeout/recovery for a DMA transfer that remains busy
forever. Extend and test fault recovery before relying on logging for faults.

The buffer is in ordinary static SRAM; the tested Debug ELF placed it at
`0x2000007c`. Verify the linker map again after changing memory sections: STM32
DMA cannot access CCM RAM. At 115200 baud, 8-N-1, peak theoretical throughput
is about 11,520 bytes/s; a 10 KB backlog is approximately 0.89 s. Persistent
generation above line rate eventually drops data. With
`configUSE_NEWLIB_REENTRANT=1`, `snprintf`/`vsnprintf` still require review of
newlib locks, heap and task stack usage when used concurrently. Avoid assuming
float formatting is enabled in the nano C library.

## Test and current limits

In PowerShell from the repository root:

```powershell
.\tools\serial-monitor.ps1 -Port COM3 -BaudRate 115200
.\tools\serial-monitor.ps1 -Port COM3 -DurationSeconds 5 -Hex
```

Only one program can open COM3 at a time. The logger was built in Debug and
Release and flashed/verified with ST-LINK. The serial monitor received lines
such as `[8500 ms] SmartSwitch: USART6 DMA and LED heartbeat` every 500 ms.
The LED GPIO was observed toggling. This verifies basic DMA operation, not
buffer-wrap correctness under load, concurrent producer behavior, overflow
accounting, DMA failure recovery, or runtime stack margin. Test those cases
before depending on it for network bring-up; inspect heap and high-water marks.

`MX_ETH_Init` still returns early at boot. Ethernet is now initialized later
by the temporary PHY/ping bring-up code, which hands MAC ownership from the HAL
probe to CycloneTCP. See `docs/ethernet-bringup.md` for the tested setup and
remaining limitations. The user is testing a populated board with relays not
yet fitted; avoid assuming relay polarity or other hardware behavior from
labels alone.
