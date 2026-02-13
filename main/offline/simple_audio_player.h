#ifndef SIMPLE_AUDIO_PLAYER_H
#define SIMPLE_AUDIO_PLAYER_H

#include <string>
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

private:
    AudioService* audio_service_;
};

} // namespace offline

#endif // SIMPLE_AUDIO_PLAYER_H
