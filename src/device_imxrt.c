#include "device.h"
#include "log.h"

#if MG_DEVICE == MG_DEVICE_RT1020 || MG_DEVICE == MG_DEVICE_RT1060

#include <flexspi.h>
#define FLEXSPI_NOR_INSTANCE 0

static bool s_flash_irq_disabled;

MG_IRAM void *mg_flash_start(void) {
  return (void *) 0x0;
}
MG_IRAM size_t mg_flash_size(void) {
  return 8 * 1024 * 1024;
}
MG_IRAM size_t mg_flash_sector_size(void) {
  return 4 * 1024;  // 4k
}
MG_IRAM size_t mg_flash_write_align(void) {
  return 256;
}
MG_IRAM int mg_flash_bank(void) {
  return 0;
}

MG_IRAM static bool flash_page_start(volatile uint32_t *dst) {
  char *base = (char *) mg_flash_start(), *end = base + mg_flash_size();
  volatile char *p = (char *) dst;
  return p >= base && p < end && ((p - base) % mg_flash_sector_size()) == 0;
}

#if MG_DEVICE == MG_DEVICE_RT1020
MG_IRAM static int flexspi_nor_get_config(flexspi_nor_config_t *config) {
  flexspi_nor_config_t default_config = {
      .memConfig = {.tag = FLEXSPI_CFG_BLK_TAG,
                    .version = FLEXSPI_CFG_BLK_VERSION,
                    .readSampleClkSrc = 1,  // ReadSampleClk_LoopbackFromDqsPad
                    .csHoldTime = 3,
                    .csSetupTime = 3,
                    .controllerMiscOption = MG_BIT(4),
                    .deviceType = 1,  // serial NOR
                    .sflashPadType = 4,
                    .serialClkFreq = 7,  // 133MHz
                    .sflashA1Size = 8 * 1024 * 1024,
                    .lookupTable = __FLEXSPI_QSPI_LUT},
      .pageSize = 256,
      .sectorSize = 4 * 1024,
      .ipcmdSerialClkFreq = 1,
      .blockSize = 64 * 1024,
      .isUniformBlockSize = false};

  *config = default_config;
  return 0;
}
#else
MG_IRAM static int flexspi_nor_get_config(flexspi_nor_config_t *config) {
  uint32_t options[] = {0xc0000008, 0x00};
  uint32_t status =
      flexspi_nor->get_config(FLEXSPI_NOR_INSTANCE, config, options);
  if (status) {
    MG_ERROR(("Failed to extract flash configuration: status %u", status));
  }
  return status;
}
#endif

uint32_t mg_flash_init() {
  flexspi_nor_config_t config;
  if (flexspi_nor_get_config(&config) != 0) {
    return false;
  }
  return flexspi_nor->init(FLEXSPI_NOR_INSTANCE, &config);
}

MG_IRAM bool mg_flash_erase(void *addr) {
  flexspi_nor_config_t config;
  if (flexspi_nor_get_config(&config) != 0) {
    return false;
  }
  if (flash_page_start(addr) == false) {
    MG_ERROR(("%p is not on a sector boundary", addr));
    return false;
  }
  // Note: Interrupts must be disabled before any call to the ROM API on RT1020
  if (!s_flash_irq_disabled) {
    asm("cpsid i");
  }
  bool ok = (flexspi_nor->erase(FLEXSPI_NOR_INSTANCE, &config, (uint32_t) addr,
                                mg_flash_sector_size()) == 0);
  if (!s_flash_irq_disabled) {
    asm("cpsie i");  // // Reenable them after the call
  }
  MG_DEBUG(("Sector starting at %p erasure: %s", addr, ok ? "ok" : "fail"));
  return ok;
}

MG_IRAM bool mg_flash_swap_bank() {
  return true;
}

static inline void spin(volatile uint32_t count) {
  while (count--) (void) 0;
}

// Note: This is the equivalent of uart_write_byte for LPUART2 (0x40188000u).
// We use this because sometimes, when we read from the memory mapped
// flash memory, the system crashes.
static inline void read_init(void) {
  *(uint32_t *) (0x40188000u + 0x1c) = 0;
  while ((*(uint32_t *) (0x40188000u + 0x14) & 0x800000U) == 0) {
    spin(1);
  }
}

MG_IRAM bool mg_flash_write(void *addr, const void *buf, size_t len) {
  flexspi_nor_config_t config;
  if (flexspi_nor_get_config(&config) != 0) {
    return false;
  }
  if ((len % mg_flash_write_align()) != 0) {
    MG_ERROR(("%lu is not aligned to %lu", len, mg_flash_write_align()));
    return false;
  }
  uint32_t *dst = (uint32_t *) addr;
  uint32_t *src = (uint32_t *) buf;
  uint32_t *end = (uint32_t *) ((char *) buf + len);
  bool ok = true;

  // Note: If we overwrite the flash irq section of the image, we must also
  // make sure interrupts are disabled and are not reenabled until we write
  // this sector with another irq table.
#ifdef RUNINFLASH
  if ((char *) addr == (char *) MG_FLASH_CODE_OFFSET) {
    s_flash_irq_disabled = true;
    asm("cpsid i");
  }
#endif

  while (ok && src < end) {
    if (flash_page_start(dst) && mg_flash_erase(dst) == false) {
      break;
    }
    uint32_t status;
    if ((char *) buf >= (char *) MG_FLASH_OFFSET) {
      // If we copy from FLASH to FLASH, then we first need to copy the source
      // to RAM
      size_t tmp_buf_size = mg_flash_write_align() / sizeof(uint32_t);
      uint32_t tmp[tmp_buf_size];
      for (size_t i = 0; i < tmp_buf_size; i++) {
        if (i == 0 && ((char *) addr >
                       (char *) (MG_FLASH_OFFSET + MG_FLASH_CODE_OFFSET))) {
          read_init();  // Ensure successful read from memory mapped flash
        }
        tmp[i] = src[i];
      }
      if (!s_flash_irq_disabled) {
        asm("cpsid i");
      }
      status = flexspi_nor->program(FLEXSPI_NOR_INSTANCE, &config,
                                    (uint32_t) dst, tmp);
    } else {
      if (!s_flash_irq_disabled) {
        asm("cpsid i");
      }
      status = flexspi_nor->program(FLEXSPI_NOR_INSTANCE, &config,
                                    (uint32_t) dst, src);
    }
    if (!s_flash_irq_disabled) {
      asm("cpsie i");
    }
    src = (uint32_t *) ((char *) src + mg_flash_write_align());
    dst = (uint32_t *) ((char *) dst + mg_flash_write_align());
    if (status != 0) {
      ok = false;
    }
  }
  MG_DEBUG(("Flash write %lu bytes @ %p: %s.", len, dst, ok ? "ok" : "fail"));

#ifdef RUNINFLASH
  if ((char *) addr == (char *) MG_FLASH_CODE_OFFSET) {
    s_flash_irq_disabled = false;
    asm("cpsie i");
  }
#endif

  return ok;
}

MG_IRAM void mg_device_reset(void) {
  MG_DEBUG(("Resetting device..."));
  *(volatile unsigned long *) 0xe000ed0c = 0x5fa0004;
}

#if MG_DEVICE == MG_DEVICE_RT1060
MG_IRAM bool mg_flash_erase_all(void) {
  flexspi_nor_config_t config;
  if (flexspi_nor_get_config(&config) != 0) {
    return false;
  }
  if (!s_flash_irq_disabled) {
    asm("cpsid i");
  }
  bool ok = (flexspi_nor->erase_all(FLEXSPI_NOR_INSTANCE, &config) == 0);
  if (!s_flash_irq_disabled) {
    asm("cpsie i");
  }
  MG_DEBUG(("Chip erase: %s", ok ? "Success" : "Failure"));
  return ok;
}
#endif

#endif
