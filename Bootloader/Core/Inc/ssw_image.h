#ifndef SSW_IMAGE_H
#define SSW_IMAGE_H
/* SmartSwitch firmware image format and boot record (docs/phase5b-bootloader.md).
 * IDENTICAL COPY in Bootloader/Core/Inc and F407_smartswitch/Core/Inc: change both. */
#include <stdint.h>

/* ---- internal flash map ---- */
#define SSW_BL_ADDR          0x08000000UL      /* bootloader, sectors 0-1 (32 KiB) */
#define SSW_APP_ADDR         0x08008000UL      /* header (512 B) then the application */
#define SSW_APP_VECTOR_OFF   0x200UL           /* vector table at 0x08008200 (512-B aligned) */
#define SSW_APP_MAX          (0x08100000UL - SSW_APP_ADDR)   /* 992 KiB incl. header */

/* ---- external flash, chip 2 (W25Q128) ---- */
#define SSW_SLOT_STAGING     0x000000UL        /* uploaded image (header + payload) */
#define SSW_SLOT_BACKUP      0x100000UL        /* last known-good image */
#define SSW_SLOT_SIZE        0x100000UL
#define SSW_BOOTREC_A        0x200000UL
#define SSW_BOOTREC_B        0x201000UL

/* ---- image header: first 512 bytes of every image ---- */
#define SSW_MAGIC_RELEASE    0x46575353UL      /* "SSWF": filled by tools/make_ssw.py, SHA-256 valid */
#define SSW_MAGIC_DEV        0x44575353UL      /* "SSWD": as linked (debugger/ST-LINK), no hash */
#define SSW_HEADER_VERSION   1U
#define SSW_HEADER_SIZE      512U
#define SSW_BOARD            "SSW-F407"

typedef struct {
  uint32_t magic;            /* SSW_MAGIC_RELEASE or SSW_MAGIC_DEV */
  uint16_t header_version;   /* SSW_HEADER_VERSION */
  uint16_t header_size;      /* 512 */
  char     board[16];        /* SSW_BOARD, zero padded */
  uint32_t fw_version;       /* major << 16 | minor << 8 | patch */
  char     fw_build[32];     /* build id (date/time or git), zero terminated */
  uint32_t load_addr;        /* SSW_APP_ADDR */
  uint32_t vector_offset;    /* SSW_APP_VECTOR_OFF */
  uint32_t image_end;        /* absolute end address of the image in internal flash */
  uint32_t payload_size;     /* bytes after the header (release images) */
  uint8_t  sha256[32];       /* SHA-256 of the payload (release images) */
  uint32_t flags;            /* reserved, 0 */
  uint8_t  signature[64];    /* reserved for a future Ed25519 signature, zeros */
  uint8_t  reserved[512 - 4 - 2 - 2 - 16 - 4 - 32 - 4 - 4 - 4 - 4 - 32 - 4 - 64 - 4];
  uint32_t header_crc;       /* CRC-32 (IEEE) of the first 508 bytes */
} SswHeader;

_Static_assert(sizeof(SswHeader) == SSW_HEADER_SIZE, "SswHeader must be 512 bytes");

/* ---- boot record (A/B copies in chip 2, highest valid sequence wins) ---- */
#define SSW_BOOTREC_MAGIC    0x52425353UL      /* "SSBR" */

typedef enum {
  SSW_BR_IDLE = 0,           /* run the internal application */
  SSW_BR_PENDING = 1,        /* install the staging image */
  SSW_BR_INSTALLING = 2,     /* backup done, programming internal flash (resume after power loss) */
  SSW_BR_TRIAL = 3,          /* new image runs, not confirmed yet */
  SSW_BR_ROLLBACK = 4,       /* restore the backup image */
} SswBootState;

typedef enum {
  SSW_RES_NONE = 0,
  SSW_RES_COMMITTED = 1,     /* trial image confirmed by the application */
  SSW_RES_ROLLED_BACK = 2,   /* trial image failed to confirm, backup restored */
  SSW_RES_INSTALL_FAILED = 3,/* staging image invalid or programming failed, old image kept */
  SSW_RES_RESTORED = 4,      /* user-requested rollback done */
  SSW_RES_REPAIRED = 5,      /* internal image was corrupt and was restored */
} SswResult;

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t state;            /* SswBootState */
  uint32_t result;           /* SswResult of the last operation */
  uint32_t result_version;   /* fw_version concerned by result */
  uint32_t trial_boots;      /* boots of the trial image so far */
  uint32_t backup_size;      /* bytes in the backup slot (whole image incl. header), 0 = none */
  uint8_t  backup_sha[32];   /* SHA-256 of those bytes, written by the bootloader after the copy */
  uint32_t crc;              /* CRC-32 of the first 60 bytes */
} SswBootRecord;

_Static_assert(sizeof(SswBootRecord) == 64, "SswBootRecord must be 64 bytes");

#define SSW_TRIAL_MAX_BOOTS  3U   /* a trial image that resets this often without confirming is rolled back */

#endif
