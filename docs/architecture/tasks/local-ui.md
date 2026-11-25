# Local UI task

> **สถานะ 2026-11-25:** ส่วนแสดงผล (LED + OLED) ทำแล้วใน `local_ui.c` ดู `docs/local-ui-status.md`
> ปุ่มยังใช้แค่ปลุกจอ/เปลี่ยนหน้า การสั่งรีเลย์จากปุ่ม (D3) ยังไม่ได้ทำ

## หน้าที่
ให้ช่างหน้างานดูสถานะรีเลย์และสั่งเปิด/ปิดด้วยปุ่ม (S2-6) เป็น adapter: ส่งคำสั่งเข้า
Control (`source = SRC_LOCAL`) และอ่าน snapshot ห้ามเขียน PCA เอง

## Owns
| Resource | หมายเหตุ |
|---|---|
| OLED SSD1306 **128×32** (I2C1 `0x3C`) | ผ่าน driver [i2c-bus](../drivers/i2c-bus.md) ไฟจาก +3V3_MCU ประหยัดไฟด้วย Display OFF (0xAE) + ปิด charge pump |
| SW1 PA15, SW2 PB5 | active-low, debounce hardware 10k/100 nF |
| LED_Blue PC11 | — |

## กฎที่กระทบ architecture
- OLED init ได้**หลัง**ที่ Control อ่าน PCA ครั้งแรกเสร็จแล้วเท่านั้น (S6-1)
- ส่งเฟรม (512 bytes ≈ 13 ms ที่ 400 kHz) เป็น chunk ≤ 128 bytes แล้วปล่อย bus lock ระหว่าง chunk (S3-2)
- OLED error ต้องไม่ทำให้ bus หยุด ให้ข้ามจอไปแล้วลองใหม่ทีหลัง
- EXTI ของปุ่มใช้แค่ปลุก task การจับเวลากดค้างใช้ polling 10 ms ตอนที่ active
- ไม่มีไฟ 24 V → Control จะปฏิเสธ OFF จอต้องแสดงเหตุผล (`NO 24V`)
- แสดงสถานะจริง (`effective`, `contact`) ไม่ใช่แค่ค่า latch, แสดง IP และ MAC (S7-2)

## UX (ยังไม่ได้ออกแบบ, open-items D3)
ข้อเสนอเดิมไว้เป็นจุดเริ่มต้น: SW1 กดสั้นเลือกช่อง, SW2 กดค้างเพื่อยืนยัน (ไป OFF 3 s, ไป ON 1 s),
การกดครั้งแรกตอนจอดับแค่ปลุกจอ, ยกเลิกการเลือกหลัง 10 s, ปิดจอหลัง 60 s ต้องจัดหน้าใหม่ให้พอดี 128×32

## เวลา
priority ต่ำ (เหนือ Diagnostics)

## การทดสอบ (gate ของ Phase 6)
- OLED ถูกถอด/เสีย → รีเลย์ยังสั่งได้, I2C recovery ทำงาน
- กดปุ่มรัว/bounce ไม่ทำให้เกิดคำสั่งซ้ำ
