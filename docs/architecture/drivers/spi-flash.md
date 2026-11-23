# Driver: SPI flash (`w25q128.c`)

ไม่ใช่ task เป็น library ที่ Storage เรียกใช้ (และ bootloader ใช้แบบ exclusive ก่อน scheduler)
รายละเอียดของโค้ดปัจจุบันอยู่ใน `docs/flash-bringup.md`

## Owns
SPI2 (PB10 SCK, PB14 MISO, PB15 MOSI, mode 0, 5.25 MHz), CS1 PD7, CS2 PD13,
DMA1 Stream 3/4, SPI2 IRQ, callback ที่ลงทะเบียนกับ handle (`USE_HAL_SPI_REGISTER_CALLBACKS`, ทำแล้ว)

## กฎ
- mutex เดียวคุมทั้งสองชิป buffer DMA เป็น static ใน SRAM
- ถือ lock ระหว่าง poll WIP ตอน program/erase (sector erase ใช้เวลาหลายร้อย ms)
- `ProgramPage` ≤ 256 bytes ห้ามข้ามหน้า, `EraseSector` ต้อง align 4 KiB, ไม่มี chip erase
- timeout ของ program/erase = สถานะไม่แน่นอน ผู้เรียกต้อง verify
- ห้ามใช้ `FlashProbe_TestBlankSectors` แบบ blocking เก่าคู่กับ driver นี้
- ห้าม erase พื้นที่ที่ยังไม่ได้กำหนดเป็นของใคร (ดู [tasks/storage.md](../tasks/storage.md))
