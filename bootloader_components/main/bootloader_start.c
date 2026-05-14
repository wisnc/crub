#include "sdkconfig.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define FACTORY_INDEX  (-1)
#define TEST_APP_INDEX (-2)

#define RESET_REASON_POWERON     0x01
#define RESET_REASON_RTC_SW      0x03
#define RESET_REASON_DEEPSLEEP   0x05
#define RESET_REASON_TG0WDT      0x07
#define RESET_REASON_TG1WDT      0x08
#define RESET_REASON_RTCWDT      0x09
#define RESET_REASON_CPU0_TG0WDT 0x0B
#define RESET_REASON_CPU0_SW     0x0C
#define RESET_REASON_CPU0_RTCWDT 0x0D
#define RESET_REASON_BROWNOUT    0x0F
#define RESET_REASON_SYS_RTCWDT  0x10
#define RESET_REASON_SYS_SUPWDT  0x12
#define RESET_REASON_USB_UART    0x15
#define RESET_REASON_USB_JTAG    0x16

static const char *TAG = "boot";

static bool is_hardware_reset(int reason)
{
    switch (reason) {
        case RESET_REASON_POWERON:
        case RESET_REASON_BROWNOUT:
        case RESET_REASON_USB_UART:
        case RESET_REASON_USB_JTAG:
            return true;
        default:
            return false;
    }
}

void __attribute__((noreturn)) call_start_cpu0(void)
{
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    bootloader_state_t bs = {0};
    int boot_index = TEST_APP_INDEX;

    if (!bootloader_utility_load_partition_table(&bs)) {
        ESP_LOGE(TAG, "partition table load failed");
        bootloader_reset();
    }

    int reason = esp_rom_get_reset_reason(0);
    ESP_LOGI(TAG, "reset reason: %d", reason);

    if (is_hardware_reset(reason)) {
        ESP_LOGI(TAG, "hardware reset → booting test (launcher)");
        boot_index = TEST_APP_INDEX;
    } else {
        ESP_LOGI(TAG, "software reset → checking OTA");
        boot_index = bootloader_utility_get_selected_boot_partition(&bs);

        if (boot_index == FACTORY_INDEX || boot_index < TEST_APP_INDEX) {
            ESP_LOGI(TAG, "no valid OTA, falling back to test");
            boot_index = TEST_APP_INDEX;
        }
    }

    bootloader_utility_load_boot_image(&bs, boot_index);

    ESP_LOGE(TAG, "boot failed");
    bootloader_reset();
}
