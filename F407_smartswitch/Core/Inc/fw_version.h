#ifndef FW_VERSION_H
#define FW_VERSION_H
/* Application version, carried in the image header (ssw_image.h). Bump for every release. */
#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 8
#define FW_VERSION_PATCH 0
#define FW_VERSION ((FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)
#define FW_VERSION_STR "0.8.0"
#endif
