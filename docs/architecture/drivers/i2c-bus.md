# Driver: I2C bus (`i2c_bus.c`) + PCA9536 (`pca9536.c`)

ไม่ใช่ task เป็น library ที่ Control (PCA9536) และ Local UI (OLED) เรียกใช้ รายละเอียดของโค้ดปัจจุบันอยู่ใน
`docs/pca9536-bringup.md`

## Owns
I2C1 (PB6 SCL / PB7 SDA, 400 kHz), DMA1 Stream 0 (RX) / Stream 7 (TX), I2C1 EV/ER IRQ,
callback ที่ลงทะเบียนกับ handle (`USE_HAL_I2C_REGISTER_CALLBACKS`, ทำแล้ว)

## อุปกรณ์บน bus
| Address | อุปกรณ์ | ใครใช้ |
|---|---|---|
| `0x41` | PCA9536 (ไม่มีขา RESET, ไม่ reset ตาม MCU) | Control เท่านั้น |
| `0x3C` | SSD1306 OLED 128×32 | Local UI เท่านั้น |

## กฎ
- mutex เดียวคุมทั้ง bus มี transaction เดียวในเวลาเดียว ทุก wait มี timeout และปล่อย lock ทุกเส้นทาง
- PCA driver ถือ lock ตลอด read-modify-write
- OLED ส่ง chunk ≤ 128 bytes แล้วปล่อย lock ระหว่าง chunk เพื่อให้ Control แทรกได้ภายใน ~3 ms
- ห้ามใครลงทะเบียน callback ของ I2C1 นอกจาก driver นี้

## Recovery (S3-2) — ทำแล้วใน Phase 1
- `I2cBus_Recover()`: abort DMA → I2C1 SWRST → `HAL_I2C_DeInit` → GPIO toggle SCL 9 ครั้ง + STOP →
  `HAL_I2C_Init` (DMA/IRQ กลับมา) → **ลงทะเบียน callback ใหม่** (DeInit/Init reset callback เป็น default)
- ไม่เขียน register ของ slave ตัวไหน จึงไม่ผิด I3 ผู้เรียกต้องอ่านกลับเองหลัง fault
- timeout แล้ว `faulted = true` → `I2cBus_Lock` ครั้งถัดไปทำ recovery เอง (ไม่เกิน 1 ครั้ง/วินาที) ถ้ายังไม่สำเร็จคืน `I2C_BUS_FAULTED` แล้วลองใหม่ครั้งหน้า
- เรียกเป็นขั้นแรกของ boot ก่อนอ่าน PCA (S6-1) นับจำนวนด้วย `I2cBus_RecoveryCount()`
- ยังไม่ได้ทดสอบกับ fault จริง (ช็อต SDA) ดู `docs/phase1-foundation.md`

## ยังต้องเพิ่ม
- helper ปัจจุบันส่งได้แค่ register 1 byte ต้องเพิ่ม API สำหรับข้อมูลหลาย byte ของ OLED
- ถ้าให้ CubeMX เปิด I2C1 EV/ER IRQ ภายหลัง ต้องเอา handler ที่เขียนเองออกก่อน และคง priority 5
