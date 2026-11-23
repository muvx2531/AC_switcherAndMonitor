# W25Q128 SPI flash bring-up

On 2026-11-19 the populated board was tested through SPI2. PB10 is SCK,
PB14 MISO, PB15 MOSI; CS1 is PD7 and CS2 is PD13. SPI mode 0, 8-bit, MSB
first, APB1/8 = 5.25 MHz. Both CS pins idle high. Normal-mode SPI2 RX/TX
DMA is configured by CubeMX. The initial probe used blocking HAL SPI; the
separate `w25q128.c` driver and DMA tests are described below.

Both chips returned JEDEC `EF 40 18` and status registers `00 02 60` on
this board. `flash_probe.c` reads those on startup and inspects the final
4 KiB sector, `0xFFF000..0xFFFFFF`, without writing. The readings are not a
full memory scan and do not prove any other sector is unused.

A one-shot program/erase test was run after confirming both entire final
sectors were all `FF`: a 256-byte pattern was programmed to the first page
of each sector, read back and compared; the corresponding page on the other
chip was checked blank; each tested sector was erased and all 4096 bytes read
back as `FF`. Both chips passed all checks. JEDEC identity, isolation and blank
checks are prerequisites, and the test exits on failure. This test wears the
sector and the chip can retain test data if interrupted or erase fails.

**Do not run the destructive test on a sector containing user data.** The
old blocking `FlashProbe_TestBlankSectors` is retained for reference but no
longer called by main. It has no SPI mutex. Do not use it while the DMA driver
or another SPI2 client is active.

After the test, ping and the temporary TCP echo tests still passed. Debug and
Release builds passed with zero reported errors and warnings. Not yet tested:
concurrent SPI clients, other flash addresses and boundaries, power-loss
behavior, protection handling, endurance, or logging integrity.
Follow the actual chip datasheet and use timeouts for all future writes.

## W25Q128 DMA driver

`Core/Inc/w25q128.h` and `Core/Src/w25q128.c` provide a shared SPI2 bus driver
for both chips. The default task calls `W25Q128_Init(&hspi2)` after scheduler
startup; initialize exactly once before other tasks use it. `main.c` then uses
the DMA driver to read and log both chip IDs. The API is task-context only.

```c
uint8_t id[3];
W25Q128_Result r = W25Q128_ReadId(W25Q128_CHIP_1, id);
uint8_t data[32];
if (r == W25Q128_OK)
    r = W25Q128_Read(W25Q128_CHIP_1, 0x1000, data, sizeof(data));
```

Functions return `W25Q128_Result`; always check it. `Read` handles arbitrary
nonzero lengths in 256-byte DMA chunks within the 16 MiB address range.
`ProgramPage` accepts at most 256 bytes and rejects a page boundary crossing;
it does not erase or verify data. NOR programming only changes bits 1 to 0;
callers own erase policy and read-back verification. `EraseSector` requires a
4 KiB aligned address and destroys that entire sector. No chip erase, block
erase, SFDP, reset, power-down, status-register writes or 4-byte addressing is
implemented. Successful HAL/program status alone does not prove a protected
sector changed: check protection configuration and verify read-back.

The driver selects one CS, performs each command as full-duplex normal-mode
`HAL_SPI_TransmitReceive_DMA`, then deasserts CS. An RTOS mutex serializes both
devices and protects its shared DMA-accessible static buffers; an ISR gives a
completion semaphore. Calls wait for completion with bounded timeout (they are
not asynchronous APIs), but the waiting task sleeps while DMA transfers. The
bus mutex is also held while polling WIP during program/erase, so other flash
callers may wait. It does not protect a different module that bypasses this
driver. DMA1 Stream 3/4 and SPI2 callbacks/IRQs are owned by this driver.
`USE_HAL_SPI_REGISTER_CALLBACKS` is enabled: `W25Q128_Init` registers static
TxRx-complete and error callbacks on the SPI2 handle; no module defines the
global `HAL_SPI_*Callback` functions. Future SPI2 users must share the bus
lock and must not re-register callbacks on SPI2. Built 2026-11-23; not yet
re-tested on hardware.
DMA timeout aborts the HAL SPI transaction; a program/erase timeout leaves
device state uncertain and requires caller-side recovery and verification.
Initialization is not thread-safe; do it once before concurrent use. Do not
call APIs from interrupts or before the scheduler starts.

On 2026-11-19 the actual board returned `EF 40 18` from both chips via DMA.
The debugger-triggered `FlashDmaTest_Run` rechecked both IDs and entire test
sectors blank before writing; checked page-crossing, unaligned erase and
out-of-range read rejection; programmed and verified 256 bytes per chip;
verified the other chip stayed blank; erased and verified all 4 KiB. Both
passed. DMA TX/RX buffers were in SRAM at `0x20003418` and `0x2000351c` in
that Debug ELF; verify the map if linker sections change. Ping and TCP echo
still worked afterward. Multi-task concurrency, DMA fault/timeout recovery,
stress, interrupted writes and production logging behavior remain untested.
The test changes flash and is never automatic: `flashTestRequest=1` triggers
it through a debugger. Obtain its address from the current ELF, and use only
when both final sectors are reserved and verified blank. An interrupted test
may leave data behind; do not erase anything unless ownership is established.
