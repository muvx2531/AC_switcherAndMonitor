# Temporary USART6 relay test protocol

This is a bring-up interface for a trusted physical serial connection, not a
production interface. USART6 PC6 TX / PC7 RX, 115200 baud, 8-N-1, no flow
control. The board's RX uses DMA2 Stream 1 circular, 512 bytes; TX is the
existing DMA debug logger. Only one application may open the ST-LINK VCP.

Commands and responses are ASCII `@<body>*<sum>\r\n`. The sum is the sum of
ASCII byte values in `<body>` modulo 256, encoded as two uppercase hexadecimal
digits. It excludes `@`, `*`, checksum digits, CR and LF. This is an 8-bit
additive checksum, **not a polynomial CRC** and not authentication. All hex
letters and command names must be uppercase. Newline terminates a frame; CR
before LF is accepted. Frames longer than 47 bytes are discarded. A fresh `@`
resynchronizes the parser. Unframed debug logs may occur between responses.

Body begins with a two-hex-digit caller-selected sequence, followed by a comma:

| Request body | Meaning |
|---|---|
| `01,GET` | Read PCA9536 output latch, input levels and direction |
| `02,SET,5` | Set the four output latch bits to hex `5` (IO3..IO0 = 0101) |
| `03,CLRSAFE` | Clear the boot-loop reset counter (safe mode, Phase 4); reply `03,CLRSAFE,OK`, effective at the next reset |

Examples on wire (`\r\n` follows each line):

```
@01,GET*6D
@01,OK,0,0,0*3B
@02,SET,5*DB
```

Success body is `SS,OK,O,I,C`: sequence, lower-nibble output latch, input
register and configuration. `C=0` means all four GPIOs are configured as
outputs. Read-back is electrical logic status, not proof of relay coil or
contact operation. **Since Phase 2, SET goes through the Control service**
(`docs/phase2-control.md`): bits set become one `SET_OFF` request, the other
bits one `SET_ON` request; the reply waits up to 1 s per request and then reads
the registers back. A valid SET can actuate relays immediately. Startup policy
remains: preserve pins already outputs, configure input pins as outputs with
LOW. State is **not saved across PCA9536 power loss**.

Errors use `SS,ERR,CHECKSUM`, `SS,ERR,COMMAND`, `SS,ERR,I2C,<number>` (read-back
failed) or `SS,ERR,<reason>` from Control: `NO_COIL_SUPPLY` (OFF requested without
24 V), `STATE_UNKNOWN`, `DIRECTION`, `BUSY`, `NOT_READY`, `I2C`, `FAILED`, `UNKNOWN`.
A rejected OFF part leaves the ON part unsent. An I2C failure can leave physical state uncertain.
Malformed framing with no usable sequence/checksum is silently discarded.
There is no replay/duplicate suppression; GET and whole-pattern SET are
idempotent, but re-check state after a timeout rather than assuming failure.
TX shares the best-effort debug log ring; if it is full/contended a response
can be lost. RX buffer overrun discards buffered bytes; UART RX errors stop the
receiver until reset. The parser is for low-rate diagnostics and has no
authentication, rate limit, or hardened transport; do not expose it to
untrusted equipment or use it as the final control protocol.

Use from the repository root:

```powershell
.\tools\relay-command.ps1 -Command GET -Port COM3
.\tools\relay-command.ps1 -Command SET -Value 5 -Port COM3
```

`Value` is decimal 0..15 (e.g. 12 means bit pattern 1100); the script encodes
one hexadecimal digit and validates matching sequence and response checksum.
The script opens and closes the port for each command. Shut the serial monitor
before using it. `-Sequence` and `-TimeoutMs` are optional.

2026-11-20 initial board verification: GET returned `@01,OK,0,0,0*3B`; SET 0
and subsequent GET passed; an intentionally bad checksum on SET F produced
`ERR,CHECKSUM`, and GET confirmed state remained 0. Eighty sequential GET
exchanges passed, crossing the RX DMA buffer wrap. Actual transitions under
the new protocol, overflow/fault injection, concurrent high-rate logging, and
production power-loss behavior remain to be tested. Do not assume the output
register, relay coil, and AC contact state are interchangeable.
