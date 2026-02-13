#include "sd_card_manager.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <sys/stat.h>
#include <dirent.h>

#define TAG "SDCardManager"

// SD card pin definitions
// For SDMMC mode (1-line or 4-line), boards should define in config.h:
//   SD_CMD, SD_CLK, SD_D0 (and optionally SD_D1, SD_D2, SD_D3)
// For SPI mode, boards should define:
//   SD_MOSI, SD_MISO, SD_SCK, SD_CS
//
// Fallback values for EchoEar board (SDMMC 1-line mode)
#ifndef SD_CMD
#define SD_CMD GPIO_NUM_38
#endif
#ifndef SD_CLK
#define SD_CLK GPIO_NUM_16
#endif
#ifndef SD_D0
#define SD_D0 GPIO_NUM_17
#endif

// SPI mode fallback pins (if board uses SPI instead of SDMMC)
#ifndef SD_MOSI
#define SD_MOSI GPIO_NUM_38
#endif
#ifndef SD_MISO
#define SD_MISO GPIO_NUM_17
#endif
#ifndef SD_SCK
#define SD_SCK GPIO_NUM_16
#endif
#ifndef SD_CS
#define SD_CS GPIO_NUM_8
#endif

namespace offline {

SDCardManager::SDCardManager() : mounted_(false), card_(nullptr) {
}

SDCardManager::~SDCardManager() {
    Deinitialize();
}

const char* SDCardManager::GetMountPoint() const {
#ifdef CONFIG_SD_CARD_MOUNT_POINT
    return CONFIG_SD_CARD_MOUNT_POINT;
#else
    return "/sdcard";
#endif
}

esp_err_t SDCardManager::Initialize() {
#ifndef CONFIG_USE_SD_CARD
    ESP_LOGW(TAG, "SD card support not enabled in menuconfig");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    if (mounted_) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card");

    // Default max files if not configured
    #ifndef CONFIG_SD_CARD_MAX_FILES
    #define CONFIG_SD_CARD_MAX_FILES 5
    #endif

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = CONFIG_SD_CARD_MAX_FILES,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card = nullptr;
    esp_err_t ret;

#ifdef CONFIG_SD_CARD_MODE_SDMMC
    // SDMMC mode (1-line or 4-line)
    ESP_LOGI(TAG, "Using SDMMC mode");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // Configure slot for 1-line mode (EchoEar uses 1-line SDIO)
    sdmmc_slot_config_t slot_config = {
        .clk = SD_CLK,
        .cmd = SD_CMD,
        .d0 = SD_D0,
        .d1 = GPIO_NUM_NC,
        .d2 = GPIO_NUM_NC,
        .d3 = GPIO_NUM_NC,
        .d4 = GPIO_NUM_NC,
        .d5 = GPIO_NUM_NC,
        .d6 = GPIO_NUM_NC,
        .d7 = GPIO_NUM_NC,
        .cd = GPIO_NUM_NC,
        .wp = GPIO_NUM_NC,
        .width = 1,  // 1-line mode
        .flags = 0,
    };

    // Optional: Configure additional data lines if defined (for 4-line mode)
    #ifdef SD_D1
    slot_config.d1 = SD_D1;
    slot_config.width = 4;  // Upgrade to 4-line mode
    #endif
    #ifdef SD_D2
    slot_config.d2 = SD_D2;
    #endif
    #ifdef SD_D3
    slot_config.d3 = SD_D3;
    #endif

    ret = esp_vfs_fat_sdmmc_mount(GetMountPoint(), &host, &slot_config, &mount_config, &card);
#else
    // SPI mode
    ESP_LOGI(TAG, "Using SPI mode");
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    ret = esp_vfs_fat_sdspi_mount(GetMountPoint(), &host, &slot_config, &mount_config, &card);
#endif

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                "If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    mounted_ = true;
    card_ = card;

    // Print card info
    sdmmc_card_print_info(stdout, card);

    uint64_t card_size_bytes = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
    ESP_LOGI(TAG, "SD card mounted. Size: %.2f GB", card_size_bytes / (1024.0 * 1024.0 * 1024.0));

    return ESP_OK;
}

void SDCardManager::Deinitialize() {
    if (!mounted_) {
        return;
    }

    ESP_LOGI(TAG, "Unmounting SD card");
    esp_vfs_fat_sdcard_unmount(GetMountPoint(), (sdmmc_card_t*)card_);

#ifdef CONFIG_SD_CARD_MODE_SPI
    spi_bus_free(SPI2_HOST);
#endif

    mounted_ = false;
    card_ = nullptr;
}

esp_err_t SDCardManager::ReadFile(const char* path, std::vector<uint8_t>& buffer) {
    if (!mounted_) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    FILE* file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        ESP_LOGE(TAG, "Invalid file size: %ld", file_size);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    // Read file
    buffer.resize(file_size);
    size_t read = fread(buffer.data(), 1, file_size, file);
    fclose(file);

    if (read != file_size) {
        ESP_LOGE(TAG, "Failed to read file completely. Read %zu of %ld bytes", read, file_size);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Read %zu bytes from %s", read, path);
    return ESP_OK;
}

esp_err_t SDCardManager::ReadTextFile(const char* path, std::string& content) {
    std::vector<uint8_t> buffer;
    esp_err_t ret = ReadFile(path, buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    content.assign(buffer.begin(), buffer.end());
    return ESP_OK;
}

bool SDCardManager::FileExists(const char* path) {
    if (!mounted_) {
        return false;
    }

    struct stat st;
    return (stat(path, &st) == 0);
}

long SDCardManager::GetFileSize(const char* path) {
    if (!mounted_) {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }

    return st.st_size;
}

esp_err_t SDCardManager::ListFiles(const char* dir_path, std::vector<std::string>& files) {
    if (!mounted_) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    if (!dir_path) {
        ESP_LOGE(TAG, "Invalid directory path (null)");
        return ESP_ERR_INVALID_ARG;
    }

    // Save path as string to avoid use-after-free
    std::string path_str(dir_path);

    DIR* dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path_str.c_str());
        return ESP_ERR_NOT_FOUND;
    }

    files.clear();
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        files.push_back(entry->d_name);
    }

    size_t file_count = files.size();
    closedir(dir);

    ESP_LOGI(TAG, "Found %d files in directory", (int)file_count);
    return ESP_OK;
}

} // namespace offline
