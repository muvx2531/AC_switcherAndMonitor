# Storage task

## หน้าที่
เป็นคนเดียวที่เขียน W25Q128 ทั้งสองชิป เก็บ event log และ config แบบมี version
และรายงานว่าข้อมูลถูกบันทึกอย่างถาวรแล้วหรือยัง

## Owns
| Resource | หมายเหตุ |
|---|---|
| W25Q128 ×2 ผ่าน driver [spi-flash](../drivers/spi-flash.md) | SPI2, CS1 PD7, CS2 PD13, 16 MiB ต่อชิป |
| partition, allocator, event log, config | — |

**ข้อมูลเดิมในชิปยังไม่รู้ว่ามีอะไร ห้ามสมมติว่าว่าง ห้าม erase จนกว่าจะกำหนด partition และตรวจแล้ว**

## Partition (ข้อเสนอ รอยืนยัน, open-items D8)
| ชิป | พื้นที่ | ใช้ทำ |
|---|---|---|
| 1 | config A / config B | config แบบ A/B มี version + CRC, เขียนสลับเพื่อให้ atomic |
| 1 | event log | ring ของ record ขนาดคงที่ มี sequence + CRC |
| 2 | firmware staging | สำหรับ bootloader/FOTA ในอนาคต (S7-3) |
| 2 | reserved | — |

ขนาดจริงกำหนดตอน implement Phase 5 เก็บ sector สุดท้ายของแต่ละชิปไว้สำหรับทดสอบตามเดิม

## API
- `Storage_Submit(record, durability)` → `QUEUED / BUSY` ส่วน completion แยกเป็น `COMMITTED / FAILED`
- คิวรับแล้ว ≠ บันทึกถาวร ยืนยันว่าถาวรหลัง verify เท่านั้น
- config อ่านเป็น snapshot ที่ไม่เปลี่ยน การแก้ต้องผ่าน validate แล้ว commit

## กฎ
- คิวเต็ม: แยก event สำคัญ (เปลี่ยนสถานะรีเลย์, กฎ (ค), fault) ออกจากค่าวัดธรรมดา ใช้ counter/aggregation ลดการสึกของ flash
- ทุก program/erase มี timeout แล้วอ่านกลับ ถ้า timeout ถือว่าสถานะไม่แน่นอน
- BACKUP → Power ขอ flush ทำแบบมีขอบเขตเวลา ห้ามรอตลอดไป
- `pending_restore` **ไม่ผ่าน Storage** (อยู่ใน BKP ของ Control)
- ห้ามคืนสถานะรีเลย์จาก flash (I4)

## เวลา
priority ต่ำกว่า Network sector erase ใช้เวลาได้ถึงหลายร้อย ms และถือ SPI lock ระหว่างนั้น
(มี Storage ใช้ SPI คนเดียวจึงไม่กระทบคนอื่น)

## การทดสอบ (gate ของ Phase 5)
- ไฟดับระหว่างเขียน config และ log → boot แล้วได้ config ล่าสุดที่สมบูรณ์
- คิวเต็มแล้ว event สำคัญไม่หาย
- ทดสอบความทนของ flash ตามอัตราการเขียนที่ประมาณไว้
