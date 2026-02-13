#include "simple_audio_player.h"
#include "sd_card_manager.h"
#include "../audio/audio_service.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <random>
#include <algorithm>

#define TAG "SimpleAudioPlayer"

namespace offline {

SimpleAudioPlayer::SimpleAudioPlayer() : audio_service_(nullptr) {
}

SimpleAudioPlayer::~SimpleAudioPlayer() {
}

esp_err_t SimpleAudioPlayer::Initialize(AudioService* audio_service) {
    if (!audio_service) {
        ESP_LOGE(TAG, "Invalid audio service");
        return ESP_ERR_INVALID_ARG;
    }

    audio_service_ = audio_service;
    ESP_LOGI(TAG, "Simple audio player initialized");
    return ESP_OK;
}

esp_err_t SimpleAudioPlayer::PlayRandomFromFolder(const char* folder_path) {
    if (!audio_service_) {
        ESP_LOGE(TAG, "Audio service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    auto& sd = SDCardManager::GetInstance();
    if (!sd.IsMounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    // List files in folder
    std::vector<std::string> files;
    esp_err_t ret = sd.ListFiles(folder_path, files);
    if (ret != ESP_OK || files.empty()) {
        ESP_LOGE(TAG, "No files found in %s", folder_path);
        return ESP_FAIL;
    }

    // Filter OGG files
    std::vector<std::string> ogg_files;
    for (const auto& file : files) {
        if (file.find(".ogg") != std::string::npos || file.find(".OGG") != std::string::npos) {
            ogg_files.push_back(file);
        }
    }

    if (ogg_files.empty()) {
        ESP_LOGE(TAG, "No OGG files found in %s", folder_path);
        return ESP_FAIL;
    }

    // Pick random OGG
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, ogg_files.size() - 1);
    int idx = dist(gen);

    std::string selected_file = ogg_files[idx];
    ESP_LOGI(TAG, "Playing random OGG: %s (%d/%d)", selected_file.c_str(), idx + 1, (int)ogg_files.size());

    // Read OGG file from SD card
    std::string full_path = std::string(folder_path) + "/" + selected_file;
    std::vector<uint8_t> ogg_data;
    ret = sd.ReadFile(full_path.c_str(), ogg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read OGG file: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Read %zu bytes from SD card", ogg_data.size());

    // Create string_view for PlaySound (it expects string_view for embedded OGG)
    // We need to allocate persistent memory for the OGG data since AudioService
    // will decode it asynchronously
    static std::vector<uint8_t> playback_buffer;  // Static to keep alive during playback
    playback_buffer = std::move(ogg_data);

    std::string_view ogg_view(reinterpret_cast<const char*>(playback_buffer.data()), playback_buffer.size());
    audio_service_->PlaySound(ogg_view);

    ESP_LOGI(TAG, "OGG playback started");
    return ESP_OK;
}

} // namespace offline
