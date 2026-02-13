#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "esp_err.h"
#include <string>
#include <vector>

namespace offline {

/**
 * SD Card Manager
 * Handles SD card mounting, file operations, and audio file reading
 */
class SDCardManager {
public:
    static SDCardManager& GetInstance() {
        static SDCardManager instance;
        return instance;
    }

    // Delete copy constructor and assignment operator
    SDCardManager(const SDCardManager&) = delete;
    SDCardManager& operator=(const SDCardManager&) = delete;

    /**
     * Initialize and mount SD card
     * @return ESP_OK on success
     */
    esp_err_t Initialize();

    /**
     * Unmount SD card
     */
    void Deinitialize();

    /**
     * Check if SD card is mounted
     */
    bool IsMounted() const { return mounted_; }

    /**
     * Get mount point path
     */
    const char* GetMountPoint() const;

    /**
     * Read entire file into buffer
     * @param path File path
     * @param buffer Output buffer
     * @return ESP_OK on success
     */
    esp_err_t ReadFile(const char* path, std::vector<uint8_t>& buffer);

    /**
     * Read text file into string
     * @param path File path
     * @param content Output string
     * @return ESP_OK on success
     */
    esp_err_t ReadTextFile(const char* path, std::string& content);

    /**
     * Check if file exists
     * @param path File path
     * @return true if file exists
     */
    bool FileExists(const char* path);

    /**
     * Get file size
     * @param path File path
     * @return File size in bytes, or -1 on error
     */
    long GetFileSize(const char* path);

    /**
     * List files in directory
     * @param dir_path Directory path
     * @param files Output vector of filenames
     * @return ESP_OK on success
     */
    esp_err_t ListFiles(const char* dir_path, std::vector<std::string>& files);

private:
    SDCardManager();
    ~SDCardManager();

    bool mounted_;
    void* card_; // sdmmc_card_t or sdspi_dev_handle_t
};

} // namespace offline

#endif // SD_CARD_MANAGER_H
