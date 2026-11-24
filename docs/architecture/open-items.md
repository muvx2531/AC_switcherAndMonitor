# Open items

เรื่องที่ยังไม่ได้ตัดสินหรือรอข้อมูล ปิดรายการเมื่อได้คำตอบ แล้วย้ายผลไปที่
[decisions.md](decisions.md) และไฟล์ task ที่เกี่ยวข้อง

## รอวัดหรือตรวจบน hardware (Phase 0)

| # | เรื่อง | ทำไมสำคัญ | ผู้รับผิดชอบ |
|---|---|---|---|
| ~~H1~~ | ✅ **ปิดแล้ว 2026-11-23:** เจ้าของโปรเจคสั่ง CH3 OFF/ON ผ่านเว็บ (Phase 2 Control) แล้วยืนยันว่ารีเลย์ทำงานจริง และ latch ของ CH3 (OFF) คงอยู่ผ่าน reset ตอน flash firmware ใหม่ (I1) ช่องอื่นยังไม่ได้ทดสอบทีละช่อง | — | — |
| ~~H2~~ | ✅ **ปิดแล้ว 2026-11-24 (บอร์ดที่ 2, MAC 02:1D:51:08:57:A2):** ต่อ 24 V → `vin_detect=1`, `efuse_ilim=1`; ถอด 24 V → `efuse_ilim` ลงเป็น 0 ก่อน แล้ว `vin_detect=0` ภายใน ~1 s (poll เว็บทุก 0.5 s) **HIGH = มี 24 V** ตรงกับ `VIN_PRESENT_LEVEL` กฎ (ค) ทำงานจริง: latch CH1 ที่เป็น 1 ถูกปล่อยเป็น 0, `supply_releases` 0→1, `glitches` 0. ยังไม่ได้วัด latency ด้วย scope | — | — |
| H3 | **Schematic วงจร Vin_Detect ของ REV2** ไม่มีใน `SitePower.pdf` (export 2026-10-22) | ประเมิน RC filter/clamp และจุดต่อกับ +24Vcoil | เจ้าของโปรเจค |
| H4 | **Release time จริงของรีเลย์** (มี 1N4148 คร่อม coil ทำให้หลุดช้ากว่า datasheet) วัด +24Vcoil เทียบหน้าสัมผัสด้วย scope | ใช้ตั้งช่วงยืนยันขา 24 V หาย (S4-2) | — |
| H5 | glitch บน PB3 ตอนสั่งรีเลย์สลับรัวๆ (ดู `glitch_count`) | ตั้ง filter | — |
| ~~H6~~ | ✅ **ปิดแล้ว 2026-11-24** จาก net ใน `SitePower.pdf` (MainMCU sheet): ADC_CH0..7 = PC0, PC2, PA0, PA3, PA4, PA5, PB0, PB1 (P/N ของช่อง 1–4), ADC_CH8 = PC3 (VBAT), ADC_CH9 = PA6 (CT), DIAG_S1..4 = PD8/PD10/PD12/PD14. การจับคู่ ADC_CHx ↔ vSense_Pn/Nn บน top sheet อ่านจาก PDF text ไม่ได้ ใช้ตาม HW doc (คู่ติดกัน) ให้ยืนยันอีกครั้งตอนลง AMC3330 | — | — |
| H7 | วัด margin เวลา boot จริงก่อนถอด jumper DIS_WDT | S4-1 | — |
| H8 | knee voltage ของแบต และกำลังไฟใน BACKUP เพื่อคำนวณ X ชั่วโมง | S1-5 | — |

## รอตัดสินใจ

| # | เรื่อง | ตัวเลือก / หมายเหตุ |
|---|---|---|
| D1 | ถ้าช่างหน้างานกับ remote สั่งขัดกัน ใครชนะ | (ก) คำสั่งล่าสุดชนะ (ใช้ชั่วคราวตอนนี้) (ข) กดปุ่มแล้วล็อก remote N นาที (ค) ช่องที่ช่าง OFF ไว้ remote ON ไม่ได้ |
| D2 | remote (web) สั่ง OFF ค้างได้ หรือได้แค่ power-cycle | ตอนนี้ชุดคำสั่งเปิดให้ทุก source |
| D3 | UX ของ OLED 128×32 และปุ่ม 2 ตัว | ข้อเสนอเดิม: SW1 เลือกช่อง, SW2 กดค้าง (OFF 3 s / ON 1 s), กดครั้งแรกแค่ปลุกจอ, ปิดจอหลัง 60 s, แสดง IP/MAC |
| D4 | ลำดับ phase 0–6 ใน [migration-plan.md](migration-plan.md) | เสนอแล้ว รอยืนยัน |
| D5 | ทำ unit test บน PC สำหรับ logic ของ Control | เสนอแล้ว รอยืนยัน |
| D6 | priority และ CCM stack (S4-3, S4-4) | ใช้ตามที่เสนอจนกว่าจะคัดค้าน |
| D7 | จำนวน site และใครดูแลระยะไกล | ใช้วางแผน v2 |
| D8 | Partition ของ W25Q128 ทั้งสองชิป | ข้อเสนออยู่ใน [tasks/storage.md](tasks/storage.md) |

## ต้องแก้เอกสาร HW (`SmartSwitch_DesignDoc`)

- HSE จริงเป็น **16 MHz** (doc เขียน 25 MHz)
- flash CS จริงเป็น **PD7 / PD13** (doc เขียน PE3 / PE5)
- เพิ่ม PB3 Vin_Detect ใน pin map
- ✅ ยืนยันจาก `SitePower.pdf` 2026-11-24: **SW1 = PA15** (pin 77), SW2 = PB5, **En_3v3_AMC = PC10** (pin 78), En_3v3_Per = PE1, En_3v3_ETH = PE0 ตรงกับ `.ioc` HW doc ที่เขียน PE4/PE2 ผิด (แก้ไฟล์ task แล้ว)
- 06 §2: เปลี่ยนจาก "ทุก boot เขียน output 0x00" เป็นพฤติกรรม KeepState ตาม I1–I4 และตัดข้อความ "WDT reset แล้วคืนโหลด"

## ความเสี่ยงที่รู้อยู่

- CycloneTCP driver มีจุดรอ reset ของ MAC แบบไม่จำกัดเวลา ต้องใส่ timeout ก่อนเปิด WDT
- ยังไม่เคยทดสอบการปิด/เปิดไฟ PHY ระหว่างทำงาน (S6-4)
- ไฟ 24 V ดับสั้นกว่าเวลาที่ firmware เขียน LOW ทัน (~1–2 ms) โหลดจะดับต่อ เป็น known limitation
- PB3 เป็นขา JTDO/SWO ใช้ SWO trace ไม่ได้ ใช้ SWD ได้ปกติ
- ต้องเช็กว่า RutOS รองรับ MQTT bridge / Data to Server ตามที่ต้องการก่อนวางแผน v2
