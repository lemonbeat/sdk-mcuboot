/*
 * Copyright (c) 2021 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <bootutil/bootutil_log.h>
#include <bootutil/fault_injection_hardening.h>

#include "bootloader_flash_priv.h"
#include "soc/soc_memory_layout.h"

#if CONFIG_IDF_TARGET_ESP32
#include "esp32/rom/uart.h"
#elif CONFIG_IDF_TARGET_ESP32S2
#include "esp32s2/rom/uart.h"
#elif CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/uart.h"
#endif

#include "esp_loader.h"
#include "flash_map_backend/flash_map_backend.h"

#define ESP_LOAD_HEADER_MAGIC 0xace637d3   /* Magic is derived from sha256sum of espmcuboot */

/*
 * Load header that should be a part of application image.
 */
typedef struct image_load_header {
    uint32_t header_magic;          /* Magic for load header */
    uint32_t entry_addr;            /* Application entry address */
    uint32_t iram_dest_addr;        /* Destination address(VMA) for IRAM region */
    uint32_t iram_flash_offset;     /* Flash offset(LMA) for start of IRAM region */
    uint32_t iram_size;             /* Size of IRAM region */
    uint32_t dram_dest_addr;        /* Destination address(VMA) for DRAM region */
    uint32_t dram_flash_offset;     /* Flash offset(LMA) for start of DRAM region */
    uint32_t dram_size;             /* Size of DRAM region */
} image_load_header_t;

static int load_segment(const struct flash_area *fap, uint32_t data_addr, uint32_t data_len, uint32_t load_addr)
{
    const uint32_t *data = (const uint32_t *)bootloader_mmap((fap->fa_off + data_addr), data_len);
    if (!data) {
        BOOT_LOG_ERR("%s: Bootloader mmap failed", __func__);
        return -1;
    }
    memcpy((void *)load_addr, data, data_len);
    bootloader_munmap(data);
    return 0;
}

void esp_app_image_load(int slot, unsigned int hdr_offset)
{
    const struct flash_area *fap;
    int area_id;
    int rc;

    image_load_header_t load_header = {0};

    area_id = flash_area_id_from_image_slot(slot);
    rc = flash_area_open(area_id, &fap);
    if (rc != 0) {
        BOOT_LOG_ERR("%s: flash_area_open failed with %d", __func__, rc);
    }

    const uint32_t *data = (const uint32_t *)bootloader_mmap((fap->fa_off + hdr_offset), sizeof(image_load_header_t));
    memcpy((void *)&load_header, data, sizeof(image_load_header_t));
    bootloader_munmap(data);

    if (load_header.header_magic != ESP_LOAD_HEADER_MAGIC) {
        BOOT_LOG_ERR("Load header magic verification failed. Aborting");
        FIH_PANIC;
    }

    if (!esp_ptr_in_iram((void *)load_header.iram_dest_addr) || !esp_ptr_in_iram((void *)(load_header.iram_dest_addr + load_header.iram_size))) {
        BOOT_LOG_ERR("IRAM region in load header is not valid. Aborting");
        FIH_PANIC;
    }

    if (!esp_ptr_in_dram((void *)load_header.dram_dest_addr) || !esp_ptr_in_dram((void *)load_header.dram_dest_addr + load_header.dram_size)) {
        BOOT_LOG_ERR("DRAM region in load header is not valid. Aborting");
        FIH_PANIC;
    }

    if (!esp_ptr_in_iram((void *)load_header.entry_addr)) {
        BOOT_LOG_ERR("Application entry point (0x%x) is not in IRAM. Aborting", load_header.entry_addr);
        FIH_PANIC;
    }

    BOOT_LOG_INF("DRAM segment: start=0x%x, size=0x%x, vaddr=0x%x", load_header.dram_flash_offset, load_header.dram_size, load_header.dram_dest_addr);
    load_segment(fap, load_header.dram_flash_offset, load_header.dram_size, load_header.dram_dest_addr);

    BOOT_LOG_INF("IRAM segment: start=0x%x, size=0x%x, vaddr=0x%x", load_header.iram_flash_offset, load_header.iram_size, load_header.iram_dest_addr);
    load_segment(fap, load_header.iram_flash_offset, load_header.iram_size, load_header.iram_dest_addr);

    BOOT_LOG_INF("start=0x%x", load_header.entry_addr);
    uart_tx_wait_idle(0);
    void *start = (void *) load_header.entry_addr;
    ((void (*)(void))start)(); /* Call to application entry address should not return */
    FIH_PANIC;
}
