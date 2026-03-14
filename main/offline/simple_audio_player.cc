#include "simple_audio_player.h"
#include "sd_card_manager.h"
#include "../audio/audio_service.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <random>
#include <algorithm>
#include <vector>

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

    std::vector<std::string> ogg_files;
    esp_err_t ret = LoadOggFiles(folder_path, ogg_files);
    if (ret != ESP_OK) {
        return ret;
    }

    // Pick random OGG
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, ogg_files.size() - 1);
    size_t idx = static_cast<size_t>(dist(gen));

    ret = PlayFileByIndex(folder_path, ogg_files, idx);
    if (ret == ESP_OK) {
        last_folder_ = folder_path;
        last_files_ = std::move(ogg_files);
        last_index_ = static_cast<int>(idx);
    }
    return ret;
}

esp_err_t SimpleAudioPlayer::PlayNextInLastFolder() {
    if (!audio_service_) {
        ESP_LOGE(TAG, "Audio service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (last_folder_.empty()) {
        ESP_LOGW(TAG, "No previous folder, cannot play next");
        return ESP_ERR_INVALID_STATE;
    }

    std::vector<std::string> ogg_files;
    esp_err_t ret = LoadOggFiles(last_folder_.c_str(), ogg_files);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t next_index = 0;
    if (last_index_ >= 0 && static_cast<size_t>(last_index_) < ogg_files.size()) {
        next_index = (static_cast<size_t>(last_index_) + 1) % ogg_files.size();
    }

    ret = PlayFileByIndex(last_folder_.c_str(), ogg_files, next_index);
    if (ret == ESP_OK) {
        last_files_ = std::move(ogg_files);
        last_index_ = static_cast<int>(next_index);
    }
    return ret;
}

esp_err_t SimpleAudioPlayer::LoadOggFiles(const char* folder_path, std::vector<std::string>& ogg_files) {
    auto& sd = SDCardManager::GetInstance();
    if (!sd.IsMounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    std::vector<std::string> files;
    esp_err_t ret = sd.ListFiles(folder_path, files);
    if (ret != ESP_OK || files.empty()) {
        ESP_LOGE(TAG, "No files found in %s", folder_path);
        return ESP_FAIL;
    }

    ogg_files.clear();
    for (const auto& file : files) {
        if (file.find(".ogg") != std::string::npos || file.find(".OGG") != std::string::npos) {
            ogg_files.push_back(file);
        }
    }

    if (ogg_files.empty()) {
        ESP_LOGE(TAG, "No OGG files found in %s", folder_path);
        return ESP_FAIL;
    }

    std::sort(ogg_files.begin(), ogg_files.end());
    return ESP_OK;
}

esp_err_t SimpleAudioPlayer::PlayFileByIndex(const char* folder_path, const std::vector<std::string>& ogg_files, size_t index) {
    if (index >= ogg_files.size()) {
        ESP_LOGE(TAG, "Invalid index %u for folder %s", static_cast<unsigned>(index), folder_path);
        return ESP_ERR_INVALID_ARG;
    }

    const std::string& selected_file = ogg_files[index];
    ESP_LOGI(TAG, "Playing OGG: %s (%u/%u)", selected_file.c_str(),
             static_cast<unsigned>(index + 1), static_cast<unsigned>(ogg_files.size()));

    std::string full_path = std::string(folder_path) + "/" + selected_file;
    std::vector<uint8_t> ogg_data;
    auto& sd = SDCardManager::GetInstance();
    esp_err_t ret = sd.ReadFile(full_path.c_str(), ogg_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read OGG file: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Read %zu bytes from SD card", ogg_data.size());

    // PlaySound expects a string_view; keep the buffer alive until decoding enqueues frames.
    static std::vector<uint8_t> playback_buffer;
    playback_buffer = std::move(ogg_data);

    std::string_view ogg_view(reinterpret_cast<const char*>(playback_buffer.data()), playback_buffer.size());
    audio_service_->PlaySound(ogg_view);

    ESP_LOGI(TAG, "OGG playback started");
    return ESP_OK;
}

} // namespace offline
