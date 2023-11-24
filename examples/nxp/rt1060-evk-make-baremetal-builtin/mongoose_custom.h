#pragma once

// See https://mongoose.ws/documentation/#build-options
#define MG_ARCH MG_ARCH_NEWLIB

#ifdef RUNINFLASH
#define MG_OTA MG_OTA_FLASH
#else
#define MG_OTA MG_OTA_FLASH_FROM_RAM
#endif

#define MG_DEVICE MG_DEVICE_RT1060

#define MG_ENABLE_TCPIP 1
#define MG_ENABLE_DRIVER_IMXRT 1
#define MG_IO_SIZE 256
#define MG_ENABLE_CUSTOM_MILLIS 1
#define MG_ENABLE_CUSTOM_RANDOM 1
#define MG_ENABLE_PACKED_FS 1

