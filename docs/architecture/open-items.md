# Open items

เรื่องที่ยังไม่ได้ตัดสินหรือรอข้อมูล ปิดรายการเมื่อได้คำตอบ แล้วย้ายผลไปที่
[decisions.md](decisions.md) และไฟล์ task ที่เกี่ยวข้อง

## รอวัดหรือตรวจบน hardware (Phase 0)

| # | เรื่อง | ทำไมสำคัญ | ผู้รับผิดชอบ |
|---|---|---|---|
| H1 | **รีเลย์ทำงานจริงหรือไม่** เมื่อ 2026-11-19 เปลี่ยน IO ของ PCA ได้ แต่ไม่ได้ยินเสียงรีเลย์ ต้องวัด +24Vcoil เทียบ CoilGND, opto, driver และหน้าสัมผัส | ทุกอย่างของ Control ขึ้นกับข้อนี้ | เจ้าของโปรเจค |
| H2 | **Polarity ของ PB3 Vin_Detect**: สั่ง `BACKUP` ตอนต่อ 24 V และตอนถอด จดค่า VIN และ EFUSE (ผลวันที่ 11-21 ได้ VIN=0, EFUSE=0 แต่ไม่รู้ว่าต่อ 24 V อยู่หรือไม่) | ใช้ตัดสินกฎ (ค) | เจ้าของโปรเจค |
| H3 | **Schematic วงจร Vin_Detect ของ REV2** ไม่มีใน `SitePower.pdf` (export 2026-10-22) | ประเมิน RC filter/clamp และจุดต่อกับ +24Vcoil | เจ้าของโปรเจค |
| H4 | **Release time จริงของรีเลย์** (มี 1N4148 คร่อม coil ทำให้หลุดช้ากว่า datasheet) วัด +24Vcoil เทียบหน้าสัมผัสด้วย scope | ใช้ตั้งช่วงยืนยันขา 24 V หาย (S4-2) | — |
| H5 | glitch บน PB3 ตอนสั่งรีเลย์สลับรัวๆ (ดู `glitch_count`) | ตั้ง filter | — |
| H6 | mapping ช่อง AC ↔ ขา ADC ↔ VAClineX ↔ DIAG_Sx ตาม schematic REV2 | Acquisition ต้องใช้ | — |
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
- `.ioc` ปัจจุบันใช้ **SW1 = PA15** (doc เขียน PE4) และ **En_3v3_AMC = PC10** (doc เขียน PE2) ต้องยืนยันกับ schematic REV2 ว่าอันไหนถูก แล้วแก้ไฟล์ task ที่อ้างขาเหล่านี้ (power.md, local-ui.md)
- 06 §2: เปลี่ยนจาก "ทุก boot เขียน output 0x00" เป็นพฤติกรรม KeepState ตาม I1–I4 และตัดข้อความ "WDT reset แล้วคืนโหลด"

## ความเสี่ยงที่รู้อยู่

- CycloneTCP driver มีจุดรอ reset ของ MAC แบบไม่จำกัดเวลา ต้องใส่ timeout ก่อนเปิด WDT
- ยังไม่เคยทดสอบการปิด/เปิดไฟ PHY ระหว่างทำงาน (S6-4)
- ไฟ 24 V ดับสั้นกว่าเวลาที่ firmware เขียน LOW ทัน (~1–2 ms) โหลดจะดับต่อ เป็น known limitation
- PB3 เป็นขา JTDO/SWO ใช้ SWO trace ไม่ได้ ใช้ SWD ได้ปกติ
- ต้องเช็กว่า RutOS รองรับ MQTT bridge / Data to Server ตามที่ต้องการก่อนวางแผน v2
