# Migration plan

ลำดับ phase ที่เสนอ (รอยืนยัน, [open-items D4](open-items.md)) แต่ละ phase ทำบน branch ของตัวเอง
ผ่าน gate แล้วค่อย merge แก้ทีละ resource และกำจัด owner ที่แข่งกันก่อนเปิด service ใหม่
เก็บผลทดสอบพร้อมวันที่, build id, อุปกรณ์ และผ่าน/ไม่ผ่าน

จุดเริ่มต้น: tag `bringup-baseline` (commit `3e461f9`)

| Phase | งาน | Gate |
|---|---|---|
| **0 ตรวจ hardware** | ปิด open-items H1–H6: รีเลย์ทำงานจริง, polarity PB3, schematic Vin_Detect, release time, mapping ADC | รีเลย์ทำงานและวัดได้จริงโดยยังไม่ต่อโหลด AC |
| **0.5 Status web (อ่านอย่างเดียว)** | HTTP server + `res.c` จาก `tools/make_res.py`, `/api/status`, ADC owner ชั่วคราว, MAC จาก UID, DHCP + static สำรอง (ดู `docs/web-status-api.md`) **ทดสอบบนบอร์ดแล้ว 2026-11-23 แบบต่อตรงกับ PC (soak 3 นาที ไม่ล้มเหลว) ยังไม่ได้ทดสอบ DHCP ผ่าน RUTM51** | เปิดหน้าได้ทั้งต่อตรงกับ PC และผ่าน RUTM51 · heap ต่ำสุดไม่ลดลงต่อเนื่องเมื่อเปิดหลายนาที · web ไม่เขียน PCA |
| **1 Foundation** | RTC/BKP ไม่ reset · reset cause · WDT boot phase + Supervisor ขั้นต้น · static task + CCM stack · I2C recovery · ตั้งขา EC25/RS485 ให้ปลอดภัย **(ทำแล้ว ทดสอบบนบอร์ดตอนเสียบ jumper DIS_WDT, ดู `docs/phase1-foundation.md`)** · linker layout ที่จอง bootloader 32 KB **เลื่อนไป Phase 5** (ถ้าไม่มี bootloader บอร์ดจะ boot ไม่ขึ้น) | boot ภายใน 1.12 s เมื่อถอด jumper DIS_WDT · I2C recovery จาก SDA ถูกช็อต (ทั้งสองข้อรอคนที่หน้าบอร์ด) · ไม่มี buffer บน stack ใน DMA |
| **2 Control** | Control task ตาม [tasks/control.md](tasks/control.md) · PB3 EXTI + ยืนยัน + polling · `pending_restore` · ย้าย serial `SET` ผ่าน Control · ลบ debug hook ใน `main.c` · web sign-in + relay API **(ทำแล้ว ทดสอบ API และคำสั่งทิศปลอดภัยบนบอร์ดแล้ว ยังไม่ได้สั่ง OFF/power-cycle จริง ดู `docs/phase2-control.md`)** | latency กฎ (ค) ≤ 10 ms · reset ระหว่าง power-cycle 3 กรณี · PCA ได้ไฟใหม่ → โหลดติดทุกช่อง · unit test ผ่าน (ถ้าตกลง D5) |
| **3 Acquisition** | dual ADC + timer, RMS, VBAT, feedback เข้า Control · BACKUP/BATV อ่านจาก snapshot **(ทำแล้ว 2026-11-24 สำหรับบอร์ดที่ยังไม่ลง AMC3330: CT + VBAT + STALE ทดสอบแล้ว, ช่อง AC และ feedback เข้า Control รอ AMC3330 ดู `docs/phase3-acquisition.md`)** | ค่าตรงกับมิเตอร์บน bench ที่ปลอดภัย · STALE/DIAG ถูกต้อง |
| **4 Power + Supervisor** | MAINS/BACKUP/LOW_POWER, rail + กฎกันไฟย้อน, heartbeat, safe mode **(ทำแล้ว 2026-11-25: ตัด/จ่าย 24 V 3 รอบ ETH กลับทุกครั้ง heap ไม่รั่ว, safe mode ผ่าน; ยังไม่ได้ทดสอบ WDT reset จริงและ LOW_POWER ดู `docs/phase4-power.md`)** | ตัด/จ่าย 24 V ซ้ำ ETH กลับทุกครั้ง · WDT reset เมื่อ task ค้าง · boot loop → safe mode |
| **5 Storage + Bootloader** | partition, event log, config A/B, bootloader ขนาดเล็ก (toggle WDT จาก RAM ระหว่าง erase internal flash) | ไฟดับระหว่างเขียน config/log · bootloader ไม่แตะ I2C/PCA |
| **6 Network + Local UI** | MAC จาก UID, DHCP + static fallback, HTTP + login, ลบ TCP echo/IP ชั่วคราว · ออกแบบ UX แล้วทำ OLED/ปุ่ม | ใช้ผ่าน Tailscale ได้ · login/CSRF/rate limit · ตัดไฟ PHY ซ้ำ |

## ข้อควรระวังของ bootloader
- erase sector 128 KB ของ flash ภายในใช้เวลา 1–2 s ขึ้นไป และ CPU หยุดอ่านคำสั่งจาก flash ระหว่างนั้น
  โค้ดที่ toggle PE7 ระหว่าง erase **ต้องรันจาก RAM**
- ห้ามแตะ I2C/PCA (I1–I3)
- config, log และ firmware staging เก็บใน W25Q128 ไม่ใช้ flash ภายใน

## ความปลอดภัยระหว่างทดสอบ
ทดสอบกับโหลด AC จริงเฉพาะตามขั้นตอน bench ที่ตกลงกันแล้ว ห้ามรันการทดสอบที่เขียน/erase flash
กับพื้นที่ที่ยังไม่ได้กำหนดเจ้าของ
