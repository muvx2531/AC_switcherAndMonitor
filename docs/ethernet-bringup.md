# Ethernet bring-up

The assembled board (without relays) was connected directly to the PC's
Ethernet interface. On 2026-11-19 the PHY reported MDIO address 0 and ID
`0007:C131`; link came up after negotiation. CycloneTCP responded to five ICMP
echo requests with zero loss. Additional tests the same day: cable unplug
produced link down; software reset with cable unplugged completed and retained
link down; reconnect restored link and ping; software reset with cable attached
restored ping. A 301-packet ping run passed with 0% loss, maximum reported
round-trip 29 ms. Power-cycle and hardware fault recovery remain untested.

The test setup uses the PC at `192.168.0.222/24`, board static IP
`192.168.0.223/24`, and locally administered test MAC `02:00:00:00:00:01`.
These are temporary bring-up settings, not unique production configuration.
From the PC: `ping 192.168.0.223`. USART6 debug reports PHY ID and link state.

`Core/Src/eth_probe.c` enables PE0, waits for the PHY, uses HAL to initialize
and read MDIO, then deinitializes HAL before CycloneTCP initializes and owns
the MAC and ETH interrupt. It provides the custom board GPIO hook because the
CycloneTCP driver's weak default configures only supported evaluation boards.
The CubeMX `MX_ETH_Init()` remains bypassed at early boot; do not enable it
alongside the CycloneTCP driver. The HAL probe and CycloneTCP initializations
are sequential, not simultaneous. The CycloneTCP driver contains unbounded
hardware reset waits, so a missing clock/PHY requires future timeout and fault
handling. PHY power polarity follows the available hardware design and was
validated by the successful probe on this board.

Existing `DebugLog` uses USART6 and DMA. Both Debug and Release builds passed
after this change. Further work: link and power-cycle tests, unique MAC and
configurable IP, bounded startup/error recovery, RAM/stack and logger stress,
then TCP/HTTP. Do not use this temporary MAC/IP unchanged for multiple boards.

## Temporary TCP echo test

`tcp_echo_test.c` starts a single-client diagnostic echo server on TCP port
5000. This test service is not authenticated and is not the intended HTTP
application; it is for the isolated direct-connected bring-up setup only.
Run `tools/test-tcp-echo.ps1` from the repository root. On 2026-11-19 three
successive connections returned 5, 256 and 2048 bytes intact. After this test,
reported FreeRTOS heap was 25,320 bytes free and the minimum observed was
15,912 bytes; TCP task stack high-water mark was 246 words, network task 413
words. These are observed remaining margins, not guarantees for later loads.
The 301-packet ping run used the earlier ping-only build, before enabling the
TCP test service. Long-duration TCP, simultaneous clients, HTTP, power cycling,
and repeated link loss with active TCP connections remain untested.
