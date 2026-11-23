# Driver: debug log (`debug_log.c`)

library + worker `debugTx` ที่ Diagnostics เป็นเจ้าของ ทุก task เรียก `DebugLog_Printf` ได้
รายละเอียดของโค้ดปัจจุบันอยู่ใน `docs/debug-logging.md`

## Owns
USART6 TX, DMA2 Stream 6, ring buffer 10,240 bytes ใน SRAM, callback TX-complete ของ USART6

## กฎ
- เรียกจาก task เท่านั้น ห้ามเรียกจาก ISR
- best effort: mutex contention หรือ buffer เต็มทำให้ข้อความหาย (นับใน stats) ไม่ใช่ audit log
- `DebugLog_Printf` ใช้ stack 256 bytes ของผู้เรียก ต้องนับใน stack budget
- ห้ามส่งข้อมูลทาง USART6 เองระหว่างที่ logger เป็นเจ้าของ
- ยังไม่มี recovery เมื่อ DMA ค้าง busy ตลอดไป
