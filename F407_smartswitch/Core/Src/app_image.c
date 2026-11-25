#include "ssw_image.h"
#include "fw_version.h"

/* Image header at 0x08008000 (linker section .ssw_header). As linked it is a DEV image
 * (no hash): the bootloader starts it after a plausibility check, so ST-LINK / debugger
 * flashing keeps working. tools/make_ssw.py turns the binary into a RELEASE image
 * (magic, payload size, SHA-256, header CRC) for web updates. */
extern uint32_t _ssw_image_end;

__attribute__((section(".ssw_header"), used))
const SswHeader sswHeader = {
  .magic = SSW_MAGIC_DEV,
  .header_version = SSW_HEADER_VERSION,
  .header_size = SSW_HEADER_SIZE,
  .board = SSW_BOARD,
  .fw_version = FW_VERSION,
  .fw_build = __DATE__ " " __TIME__,
  .load_addr = SSW_APP_ADDR,
  .vector_offset = SSW_APP_VECTOR_OFF,
  .image_end = (uint32_t)&_ssw_image_end,
};
