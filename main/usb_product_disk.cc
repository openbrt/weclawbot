#include "usb_product_disk.h"

#include <cstdio>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <ff.h>
#include <tinyusb.h>
#include <tinyusb_cdc_acm.h>
#include <tinyusb_default_config.h>
#include <tinyusb_msc.h>
#include <wear_levelling.h>

#include "app_config.h"

namespace {
constexpr char kTag[] = "UsbProductDisk";
char kBasePath[] = "/wecusb";
constexpr char kReadmePath[] = "/wecusb/README.TXT";
constexpr char kVersionPath[] = "/wecusb/VERSION.TXT";
constexpr char kShortHtmlPath[] = "/wecusb/WECLAWBT.HTM";
constexpr char kLongHtmlPath[] = "/wecusb/WeClawBot.html";
constexpr char kPartitionLabel[] = "usbdrive";

tinyusb_msc_storage_handle_t g_storage = nullptr;
wl_handle_t g_wl_handle = WL_INVALID_HANDLE;

bool WriteTextFile(const char* path, const char* content) {
    FILE* file = std::fopen(path, "w");
    if (!file) {
        ESP_LOGW(kTag, "open %s failed", path);
        return false;
    }
    const bool ok = std::fputs(content, file) >= 0;
    return std::fclose(file) == 0 && ok;
}

void EnsureProductFiles() {
    const FRESULT label_result = f_setlabel("WeClawBot");
    if (label_result != FR_OK) {
        ESP_LOGW(kTag, "set FAT label failed: %d", static_cast<int>(label_result));
    }

    WriteTextFile(kReadmePath,
                  "WeClawBot / 微笺屏\n"
                  "\n"
                  "官网: " WEC_PRODUCT_URL "\n"
                  "\n"
                  "打开 WeClawBot.html 或 WECLAWBT.HTM 进入安装、配置和帮助入口。\n"
                  "也可以在微信里发送“帮助”或“官网”找回入口。\n");

    WriteTextFile(kVersionPath,
                  "name=WeClawBot\n"
                  "board=Waveshare ESP32-S3-RLCD-4.2\n"
                  "firmware=" WEC_FIRMWARE_VERSION "\n"
                  "url=" WEC_PRODUCT_URL "\n");

    constexpr char kHtml[] =
        "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>WeClawBot</title>"
        "<style>body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
        "max-width:720px;margin:48px auto;padding:0 22px;line-height:1.7;color:#17201c}"
        "a{color:#0f766e;font-weight:700}.box{border:1px solid #cfd8d3;padding:18px;margin:18px 0}</style>"
        "<h1>WeClawBot 微笺屏</h1>"
        "<p>这是设备自带的帮助入口。官网、安装、配置和文档都从这里找回。</p>"
        "<div class=\"box\"><p><a href=\"" WEC_PRODUCT_URL "\">打开 weclawbot.link</a></p>"
        "<p>如果浏览器没有自动打开链接，请复制：</p>"
        "<p><code>" WEC_PRODUCT_URL "</code></p></div>"
        "<p>微信里发送 <b>帮助</b>、<b>官网</b> 或 <b>/help</b> 也可以找回入口。</p>"
        "<p>Firmware " WEC_FIRMWARE_VERSION "</p></html>\n";

    WriteTextFile(kShortHtmlPath, kHtml);
    WriteTextFile(kLongHtmlPath, kHtml);
}

bool InitCdcAcm() {
#if CONFIG_TINYUSB_CDC_ENABLED
    tinyusb_config_cdcacm_t acm_cfg = {};
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    esp_err_t err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "tinyusb_cdcacm_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "TinyUSB CDC ACM ready");
#endif
    return true;
}

}  // namespace

bool StartUsbProductDisk() {
    if (g_storage) {
        return true;
    }

    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, kPartitionLabel);
    if (!partition) {
        ESP_LOGW(kTag, "FAT partition %s not found", kPartitionLabel);
        return false;
    }

    esp_err_t err = wl_mount(partition, &g_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "wl_mount failed: %s", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_storage_config_t storage_cfg = {};
    storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP;
    storage_cfg.medium.wl_handle = g_wl_handle;
    storage_cfg.fat_fs.base_path = kBasePath;
    storage_cfg.fat_fs.config.format_if_mount_failed = true;
    storage_cfg.fat_fs.config.max_files = 4;
    storage_cfg.fat_fs.config.allocation_unit_size = 4096;
    storage_cfg.fat_fs.format_flags = 0;

    err = tinyusb_msc_new_storage_spiflash(&storage_cfg, &g_storage);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "tinyusb_msc_new_storage_spiflash failed: %s", esp_err_to_name(err));
        return false;
    }

    EnsureProductFiles();

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    InitCdcAcm();

    err = tinyusb_msc_set_storage_mount_point(g_storage, TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "set MSC mount USB failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(kTag, "WeClawBot USB MSC disk ready");
    return true;
}
