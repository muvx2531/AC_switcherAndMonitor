# Decision log

ข้อตัดสินใจจากการ review architecture วันที่ 2026-11-23 (ผู้ตัดสิน: เจ้าของโปรเจค)
ID อ้างถึง section ของ draft เดิมที่ถูก review รายการที่มีป้าย *(เสนอ, ยังไม่ยืนยันตรงๆ)*
ใช้ตามที่เสนอไปก่อนจนกว่าจะมีคนคัดค้าน

## Relay และ invariants

| ID | ข้อตัดสินใจ | เหตุผล |
|---|---|---|
| S1-1 | Invariants I1–I3: อ่าน PCA ก่อน, ห้ามเปลี่ยน coil เอง, error แล้วห้ามเขียน (ดู [README](README.md) หัวข้อ 2) | เจ้าของโปรเจคต้องการให้สถานะรีเลย์คงเดิมผ่าน reset/ค้าง/fault ทุกกรณี และเปลี่ยนได้ด้วยคำสั่ง user เท่านั้น |
| S1-2 | router ไม่ได้ต่อผ่านรีเลย์ใน v1 การคุมไฟ 4G แยกจาก PCA9536 | ตัดกรณีร้ายที่ power-cycle router แล้ว reset ค้าง ทำให้ site ติดต่อไม่ได้ |
| S1-3 | `pending_restore` เก็บใน RTC BKP register (magic + CRC) และทำต่อหลัง reset ได้ (I2 ข้อ ข) ตอน boot ตรวจเทียบกับ PCA 3 กรณี | ถ้า reset ระหว่าง power-cycle โหลดต้องไม่ดับค้าง BKP รอดผ่าน reset และมีถ่าน CR1220 |
| S1-4 | fail-safe มีความหมายเดียว: 24 V หาย → โหลด ON (I5) ต้องแก้ HW doc 06 §2 ที่เขียนว่า WDT reset แล้วคืนโหลด | ผลของ S1-1 |
| S1-5 | v1 ใช้เกณฑ์ LOW_POWER แบบหลวม: VBAT < ~6.2 V **หรือ** อยู่บนแบต > X ชม. ค่าเป็น config | แบตยังไม่สำคัญใน v1 และ LiFePO4 ประมาณ SOC จากแรงดันไม่ได้ |
| S2-1 | Power ส่ง**ข้อเท็จจริง** (`coil_supply`, mode) ให้ Control ไม่ใช่คำสั่ง | Control เป็นคนเดียวที่ตัดสินเรื่องรีเลย์ |
| S2-2 | I2 ข้อ (ค): 24 V หาย (ยืนยันแล้ว) → latch LOW ทุกช่อง + ล้าง pending ใช้กับตอน boot ด้วย | PCA ได้ไฟจากแบต latch HIGH จึงค้าง ทำให้โหลดดับซ้ำเมื่อ 24 V กลับ |
| S2-3 | PB3 Vin_Detect ต่อกับวงจรตรวจจับ +24V **หลัง F101** ใช้เป็นสัญญาณหลักของ `coil_supply` | ครอบคลุมกรณี F101 ขาดด้วย |
| S2-4 | ไม่มีวงจรวัด AC ขาเข้า สถานะหน้าสัมผัสจึงเป็น `confirmed / inferred / unknown` | อนุมานจากแรงดันทุกช่องที่ coil ไม่มีไฟ |
| S2-5 | RS485 ไม่อยู่ใน v1: PE1 = LOW, PC9 = LOW, PD5 = input/low | ออกแบบเผื่อไว้ใน hardware |
| S2-6 | OLED SSD1306 **128×32** + SW1/SW2 ใช้ดูสถานะและสั่งเปิด/ปิดโดยช่าง เป็น adapter ที่ส่งเข้า Control รายละเอียด UX เลื่อนไปออกแบบทีหลัง | — |

## Peripheral และ concurrency

| ID | ข้อตัดสินใจ | เหตุผล |
|---|---|---|
| S3-1 | UART/I2C/SPI ใช้ HAL register callbacks ต่อ handle **(ทำแล้วใน `bringup-baseline`)** | ไม่มี symbol global ชนกัน ตรงกับหลัก owner เดียว |
| S3-2 | I2C มี recovery (toggle SCL 9 ครั้ง → STOP → re-init → ลงทะเบียน callback ใหม่ → อ่านกลับ) ไม่ latch fault ถาวร OLED ส่ง chunk ≤128 bytes | กฎ (ค) ต้องเขียน PCA ได้เสมอ และ OLED เสียต้องไม่ลามถึงรีเลย์ |
| S3-3 | ADC1+ADC2 dual regular simultaneous, timer trigger 6.4 kHz, VBAT เป็น injected ของ ADC1 ทุก 1 s (**แก้ 2026-11-24:** ใช้ rank ปกติของ ADC2 เฉลี่ย 1 s แทน ดู `docs/phase3-acquisition.md`), Acquisition เป็นเจ้าของคนเดียว | ตาม HW doc ต้อง sample P/N ของ AMC3330 พร้อมกัน |
| S3-4 | EC25 ไม่อยู่ใน v1: PD1 = LOW, PA9/PD15 = input/low | — |
| S4-1 | WDT 2 ช่วง: boot phase (toggle ระหว่าง init, มีเส้นตายรวม) แล้ว Supervisor รับต่อ | STWD100 timeout 1.12 s ปิดด้วย software ไม่ได้ ตอนนี้บอร์ดเสียบ jumper DIS_WDT อยู่ |
| S4-2 | PB3: EXTI ปลุก Control → Control ยืนยันโดยอ่านซ้ำ (ขาหาย 2–3 ms, ขากลับ 100–500 ms) → polling สำรองทุก 20–50 ms และตอน boot เป้า latency ≤10 ms | กัน noise/glitch จากการสวิตช์ coil glitch ที่สั้นกว่า release time ทำให้รีเลย์หลุดไม่ได้ ค่าจริงรอวัด |
| S4-3 | static task + stack ใน CCM ห้าม DMA จาก stack *(เสนอ, ยังไม่ยืนยันตรงๆ)* | SRAM เหลือน้อย (bss 80 KB, heap ต่ำสุด ~15.9 KB) CCM ยังว่าง |
| S4-4 | priority: Control > Acquisition ≥ Power > Supervisor > Network > Storage > Local UI > Diagnostics *(เสนอ, ยังไม่ยืนยันตรงๆ)* | debugRx ตอนนี้ +25 สูงเกิน |

## Interface

| ID | ข้อตัดสินใจ | เหตุผล |
|---|---|---|
| S5-1 | คำสั่ง `SET_ON / SET_OFF / POWER_CYCLE / CANCEL_CYCLE` ใช้ `channel_mask`, ระบุ `source`, Control เป็นคนออก `request_id` | ครอบคลุม toggle จากปุ่ม, power-cycle และ SET แบบทั้ง pattern ของ serial |
| S5-2 | power-cycle duration 1–600 s ค่า default 10 s | HW doc แนะนำ 5–10 s เพื่อให้ PSU คายประจุ |
| S5-3 | คำสั่งใหม่บนช่องที่กำลัง power-cycle **แทนที่**คำสั่งเดิม (SUPERSEDED) | — |
| S5-4 | ผู้ส่งรู้ผลด้วยการ poll ring ของ outcome (8–16 รายการ) รอยืนยัน feedback 200 ms Control ดึง snapshot จาก Acquisition เอง | ไม่ต้องมี callback ข้าม task ไม่ทำให้คิวท่วม |
| S5-5 | ไม่มีไฟ 24 V → ปฏิเสธ `SET_OFF` และ `POWER_CYCLE` (`NO_COIL_SUPPLY`) | coil จ่ายไม่ได้ และกฎ (ค) จะปล่อยทันทีอยู่แล้ว |

## Startup และ power

| ID | ข้อตัดสินใจ | เหตุผล |
|---|---|---|
| S6-1 | ลำดับ boot ตาม [tasks/init.md](tasks/init.md) I2C transaction แรกของระบบคือ recovery + อ่าน PCA | bus อาจค้างจาก reset กลาง transaction การ init OLED ก่อนอาจทำให้ latch เปลี่ยน |
| S6-2 | I4: ห้ามคืน OFF จาก flash/BKP หลัง PCA ได้ไฟใหม่ | ไฟดับหมดแล้วกลับมา = โหลดติดทุกช่อง ตรงกับ hardware |
| S6-3 | v1 มี MAINS / BACKUP / LOW_POWER LOW_POWER = ลด clock + ปิด rail ไม่ใช้ Stop mode (เลื่อนไป v2) | แบตไม่สำคัญใน v1, Stop mode เสี่ยงกับ WDT/clock/RTOS |
| S6-4 | BACKUP → **ปิด ETH เสมอ ไม่มี config** และเปิดเองเมื่อ 24 V กลับ | router ดับพร้อม mains ไม่มีกรณีที่ต้องใช้ ETH บนแบต |
| S6-5 | safe mode เมื่อ reset ≥ 5 ครั้งใน 10 นาที: เปิดแค่ Control, Local UI, Diagnostics | กัน boot loop จาก network/storage |

## Network และ update

| ID | ข้อตัดสินใจ | เหตุผล |
|---|---|---|
| S7-1 | ไม่ได้ขาย และเปิด source ทั้งหมด → ใช้ CycloneTCP ภายใต้ GPLv2 ต่อได้ | ต้องเผยแพร่ source รวมส่วนที่แก้ |
| S7-2 | MAC สร้างจาก UID 96-bit ของ STM32 (locally administered) แสดง IP และ MAC บน OLED | ทุกบอร์ดตอนนี้ใช้ `02:00:00:00:00:01` ซ้ำกัน |
| S7-3 | bootloader ขนาดเล็กตั้งแต่ v1 ใน sector 0–1 (32 KB) app เริ่มที่ sector 2 | ถ้า v1 ไม่มี bootloader จะอัปเดตผ่าน network ไม่ได้ตลอดไป |
| S7-4 | web เฉพาะใน LAN (HTTP + login) เข้าจากระยะไกลผ่าน VPN ของ RUTM51 (แนะนำ Tailscale) **ห้าม port forward** | ซิม 4G มักเป็น CGNAT และไม่ควรเปิดอุปกรณ์ที่ตัดไฟได้ออก internet |
| S7-5 | IP ผูกด้วย DHCP static lease บน router จาก MAC มี static IP สำรองตั้งผ่าน serial | MAC จาก UID คงที่ |
| S7-7 | รหัสเข้าเว็บ**ใช้รหัสเดียวกันทุกบอร์ด** (เปลี่ยนจากข้อเสนอเดิมที่ให้ไม่ซ้ำกันต่อบอร์ด) เก็บใน `web_secrets.h` ที่ไม่เข้า git; MAC ยังสร้างจาก UID ต่อบอร์ดตาม S7-2 | สะดวกต่อเจ้าหน้าที่ ข้อแลกเปลี่ยน: ถ้ารหัสรั่ว ทุกบอร์ดได้รับผลกระทบ จึงต้องพึ่ง LAN + VPN ตาม S7-4 และต้องเปลี่ยนรหัสได้โดยไม่ build ใหม่เมื่อมี Storage |
| S7-6 | remote แบบอื่น (MQTT/Modbus ผ่าน router) เป็นงาน v2 เพิ่มเป็น adapter | ไม่ต้องมี TLS บน MCU |
