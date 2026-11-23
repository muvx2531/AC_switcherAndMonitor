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

## ต้องเปลี่ยน (S3-2)
- ตอนนี้ timeout แล้ว `faulted = true` จนกว่าจะ reset → เปลี่ยนเป็น **recovery**:
  abort DMA → ตั้ง SCL เป็น GPIO แล้ว toggle 9 ครั้ง → ส่ง STOP → `HAL_I2C_DeInit` / `HAL_I2C_Init`
  → **ลงทะเบียน callback ใหม่** (DeInit/Init reset callback กลับเป็น default) → caller อ่านกลับ
- recovery ไม่เปลี่ยน latch ของ PCA จึงไม่ผิด I3
- recovery ไม่สำเร็จ N ครั้ง → รายงาน fault แต่ยังลองต่อเป็นระยะ
- recovery เป็นขั้นแรกของ boot ก่อนอ่าน PCA (S6-1)
- helper ปัจจุบันส่งได้แค่ register 1 byte ต้องเพิ่ม API สำหรับข้อมูลหลาย byte ของ OLED
- ถ้าให้ CubeMX เปิด I2C1 EV/ER IRQ ภายหลัง ต้องเอา handler ที่เขียนเองออกก่อน และคง priority 5
