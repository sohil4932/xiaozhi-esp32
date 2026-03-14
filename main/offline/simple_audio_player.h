#ifndef SIMPLE_AUDIO_PLAYER_H
#define SIMPLE_AUDIO_PLAYER_H

#include <string>
#include <vector>
#include "esp_err.h"

// Forward declarations
class AudioService;

namespace offline {

/**
 * Simple Audio Player
 * Plays random OGG files from SD card folders
 * Uses existing AudioService OGG playback infrastructure
 */
class SimpleAudioPlayer {
public:
    SimpleAudioPlayer();
    ~SimpleAudioPlayer();

    /**
     * Initialize player
     * @param audio_service Audio service for playback
     * @return ESP_OK on success
     */
    esp_err_t Initialize(AudioService* audio_service);

    /**
     * Play random OGG from folder
     * @param folder_path Folder path (e.g., "/sdcard/stories")
     * @return ESP_OK on success
     */
    esp_err_t PlayRandomFromFolder(const char* folder_path);

    /**
     * Play next OGG from the last played folder
     * @return ESP_OK on success
     */
    esp_err_t PlayNextInLastFolder();

private:
    AudioService* audio_service_;
    std::string last_folder_;
    std::vector<std::string> last_files_;
    int last_index_ = -1;

    esp_err_t LoadOggFiles(const char* folder_path, std::vector<std::string>& ogg_files);
    esp_err_t PlayFileByIndex(const char* folder_path, const std::vector<std::string>& ogg_files,
                              size_t index);
};

} // namespace offline

#endif // SIMPLE_AUDIO_PLAYER_H
