#include "afe_wake_word.h"
#include "audio_service.h"
#include <esp_log.h>
#include <sstream>
#include "esp_process_sdkconfig.h"

#define DETECTION_RUNNING_EVENT 1

#define TAG "AfeWakeWord"

AfeWakeWord::AfeWakeWord()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

AfeWakeWord::~AfeWakeWord() {
    if (multinet_preload_task_ != nullptr) {
        vTaskDelete(multinet_preload_task_);
        multinet_preload_task_ = nullptr;
        multinet_preloading_.store(false);
    }

    // Cleanup MultiNet
    if (multinet_model_ != nullptr && multinet_iface_ != nullptr) {
        multinet_iface_->destroy(multinet_model_);
        multinet_model_ = nullptr;
    }

    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }

    vEventGroupDelete(event_group_);
}

bool AfeWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
    } else {
        models_ = models_list;
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize models");
        return false;
    }
    for (int i = 0; i < models_->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
        if (strstr(models_->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models_->model_name[i];
            auto words = esp_srmodel_get_wake_words(models_, wakenet_model_);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
            }
        }
    }

    // Initialize AFE without wake word model for offline mode
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    // In both offline and online modes, use AFE without wake word.
    // Offline command model (MultiNet) is preloaded separately in background.
    // Online mode does not require local MultiNet.
    ESP_LOGI(TAG, "%s mode: Initializing AFE without wake word",
             offline_mode_enabled_ ? "Offline" : "Online");

    afe_config_t* afe_config = afe_config_init(input_format.c_str(), nullptr, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    // MultiNet is not loaded in Initialize(); it is preloaded asynchronously later.
    ESP_LOGI(TAG, "MultiNet not initialized - %s",
             offline_mode_enabled_ ? "will preload in background" : "not needed in online mode");

    xTaskCreate([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, 3, nullptr);

    return true;
}

void AfeWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void AfeWakeWord::OnCommandDetected(std::function<void(int command_id, const std::string& command_string)> callback) {
    command_detected_callback_ = callback;
}

void AfeWakeWord::OnCommandListeningChange(std::function<void(bool listening)> callback) {
    command_listening_change_callback_ = callback;
}

void AfeWakeWord::Start() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void AfeWakeWord::Stop() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}

void AfeWakeWord::InitializeMultiNet() {
    // Find MultiNet model in models list
    char* mn_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mn_name == nullptr) {
        ESP_LOGW(TAG, "No English MultiNet model found, offline commands disabled");
        return;
    }

    ESP_LOGI(TAG, "Initializing MultiNet model: %s", mn_name);
    multinet_iface_ = esp_mn_handle_from_name(mn_name);
    if (multinet_iface_ == nullptr) {
        ESP_LOGE(TAG, "Failed to get MultiNet interface");
        return;
    }

    // Create MultiNet model with 5 second timeout for command detection
    multinet_model_ = multinet_iface_->create(mn_name, 5000);
    if (multinet_model_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create MultiNet model");
        return;
    }

    // Load offline commands from sdkconfig
    ESP_LOGI(TAG, "Loading offline commands from sdkconfig:");
    esp_mn_commands_update_from_sdkconfig(multinet_iface_, multinet_model_);

    // Print active commands
    multinet_iface_->print_active_speech_commands(multinet_model_);

    ESP_LOGI(TAG, "MultiNet initialized successfully");
}

void AfeWakeWord::PreloadCommandModel(uint32_t delay_ms) {
    if (!offline_mode_enabled_) {
        ESP_LOGD(TAG, "Skip MultiNet preload: offline mode disabled");
        return;
    }

    if (multinet_model_ != nullptr) {
        ESP_LOGD(TAG, "Skip MultiNet preload: model already loaded");
        return;
    }

    bool expected = false;
    if (!multinet_preloading_.compare_exchange_strong(expected, true)) {
        ESP_LOGD(TAG, "Skip MultiNet preload: preload already in progress");
        return;
    }

    multinet_preload_delay_ms_ = delay_ms;
    auto preload_task = [](void* arg) {
        auto* this_ = static_cast<AfeWakeWord*>(arg);
        uint32_t delay = this_->multinet_preload_delay_ms_;
        if (delay > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay));
        }

        if (!this_->offline_mode_enabled_) {
            ESP_LOGI(TAG, "MultiNet preload cancelled: offline mode disabled");
        } else if (this_->multinet_model_ == nullptr) {
            ESP_LOGI(TAG, "Preloading MultiNet model in background...");
            this_->InitializeMultiNet();
        }

        if (this_->multinet_model_ != nullptr) {
            ESP_LOGI(TAG, "MultiNet preload completed");
        } else {
            ESP_LOGW(TAG, "MultiNet preload finished without a valid model");
        }

        this_->multinet_preloading_.store(false);
        this_->multinet_preload_task_ = nullptr;
        vTaskDelete(NULL);
    };

    BaseType_t created = pdFAIL;
#if CONFIG_FREERTOS_UNICORE
    created = xTaskCreate(preload_task, "mn_preload", 4096, this, 0, &multinet_preload_task_);
#else
    // Pin preload to CPU0 to reduce contention with touch/display tasks typically on CPU1.
    created = xTaskCreatePinnedToCore(preload_task, "mn_preload", 4096, this, 0, &multinet_preload_task_, 0);
#endif

    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MultiNet preload task");
        multinet_preloading_.store(false);
        multinet_preload_task_ = nullptr;
    }
}

void AfeWakeWord::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!(xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT)) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

size_t AfeWakeWord::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeWakeWord::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);

        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            continue;
        }

        // Store the wake word data for voice recognition, like who is speaking
        // Skip in offline mode to save SRAM
        if (!offline_mode_enabled_) {
            StoreWakeWordData(res->data, res->data_size / sizeof(int16_t));
        }

        // Wake word detection (skip in offline mode - no wake word loaded)
        if (!offline_mode_enabled_ && res->wakeup_state == WAKENET_DETECTED) {
            last_detected_wake_word_ = wake_words_[res->wakenet_model_index - 1];
            ESP_LOGI(TAG, "Wake word detected: %s", last_detected_wake_word_.c_str());

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }

            // Online mode: stop detection, server handles conversation via xiaozhi protocol
            Stop();
            ESP_LOGI(TAG, "Wake word detected in online mode, stopping detection for server conversation");
        }

        // Command detection (only active in offline mode when command_mode_active)
        static int cmd_frame_count = 0;
        if (offline_mode_enabled_) {
            if (command_mode_active_ && multinet_iface_ != nullptr && multinet_model_ != nullptr) {
                // Process command detection with frame skipping to prevent ringbuffer overflow
                // Skip every other frame to keep fetch() running fast enough
                if (++cmd_frame_count % 2 == 0) {
                    // Skip this frame to keep ringbuffer from filling
                    continue;
                }

                esp_mn_state_t mn_state = multinet_iface_->detect(multinet_model_, res->data);

                if (mn_state == ESP_MN_STATE_DETECTING) {
                    // Still detecting, continue listening
                    continue;
                }

                if (mn_state == ESP_MN_STATE_DETECTED) {
                    // Command detected!
                    esp_mn_results_t *mn_result = multinet_iface_->get_results(multinet_model_);
                    ESP_LOGI(TAG, "Command detected! ID: %d, String: %s, Prob: %.2f",
                        mn_result->command_id[0], mn_result->string, mn_result->prob[0]);

                    // Exit command mode first - this will clear abort flag via callback
                    cmd_frame_count = 0;  // Reset frame counter
                    StopCommandListening();  // This calls listening=false callback which clears abort flag

                    // Now trigger the command - abort flag is cleared so playback will work
                    if (command_detected_callback_) {
                        command_detected_callback_(mn_result->command_id[0], std::string(mn_result->string));
                    }

                    ESP_LOGI(TAG, "Command handled, MultiNet kept loaded for next command");
                }

                if (mn_state == ESP_MN_STATE_TIMEOUT) {
                    // Timeout - no command detected
                    ESP_LOGW(TAG, "Command detection timeout");
                    cmd_frame_count = 0;  // Reset frame counter
                    StopCommandListening();  // MultiNet remains loaded for fast next trigger
                }
            }
            // If not in command mode, just continue fetching without processing
            // This keeps the ringbuffer from overflowing during playback
        }
    }
}

void AfeWakeWord::StoreWakeWordData(const int16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 30ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 30) {
        wake_word_pcm_.pop_front();
    }
}

void AfeWakeWord::TriggerCommandListening() {
    ESP_LOGI(TAG, "TriggerCommandListening called, offline_mode: %d, command_mode_active: %d",
             offline_mode_enabled_, command_mode_active_);

    if (!offline_mode_enabled_) {
        ESP_LOGW(TAG, "Cannot trigger command listening - not in offline mode");
        return;
    }

    if (command_mode_active_) {
        ESP_LOGW(TAG, "Command listening already active");
        return;
    }

    // Notify callback FIRST - AudioService will stop playback before command mode starts.
    ESP_LOGI(TAG, "Invoking command listening callback (true)");
    if (command_listening_change_callback_) {
        command_listening_change_callback_(true);
    }

    // Give AudioService time to stop playback
    vTaskDelay(pdMS_TO_TICKS(50));

    // Try to use preloaded MultiNet first; wait briefly if preload is still running.
    ESP_LOGI(TAG, "Checking MultiNet model, multinet_model_: %p", multinet_model_);
    if (multinet_model_ == nullptr && multinet_preloading_.load()) {
        ESP_LOGI(TAG, "MultiNet preload in progress, waiting...");
        const TickType_t step = pdMS_TO_TICKS(20);
        const TickType_t timeout = pdMS_TO_TICKS(1200);
        TickType_t waited = 0;
        while (multinet_model_ == nullptr && multinet_preloading_.load() && waited < timeout) {
            vTaskDelay(step);
            waited += step;
        }
    }

    if (multinet_model_ == nullptr) {
        ESP_LOGW(TAG, "MultiNet not preloaded in time, loading synchronously");
        InitializeMultiNet();
        if (multinet_model_ == nullptr) {
            ESP_LOGE(TAG, "Failed to load MultiNet");
            // Notify callback that we failed
            if (command_listening_change_callback_) {
                command_listening_change_callback_(false);
            }
            return;
        }
    } else {
        ESP_LOGI(TAG, "MultiNet already loaded, reusing existing model");
    }

    ESP_LOGI(TAG, "Entering command listening mode");
    command_mode_active_ = true;
    multinet_iface_->clean(multinet_model_);
}

void AfeWakeWord::StopCommandListening() {
    if (!command_mode_active_) {
        return;
    }

    ESP_LOGI(TAG, "Stopping command listening");
    command_mode_active_ = false;

    // Don't stop AFE task - keep it running for next screen tap
    // The detection loop will just fetch and discard audio when not in command mode
    // This keeps the ringbuffer from overflowing

    // Notify callback that command listening stopped
    if (command_listening_change_callback_) {
        command_listening_change_callback_(false);
    }

    // Keep MultiNet loaded to prevent display freezing during reload
    // MultiNet7 initialization takes ~600ms which causes UI freeze and SPI errors
    // Trade-off: Uses ~100KB SRAM but eliminates the freeze on every tap
    // Memory usage: MultiNet7 model data stays in PSRAM, runtime ~100KB in SRAM
    if (multinet_model_ != nullptr && multinet_iface_ != nullptr) {
        ESP_LOGI(TAG, "MultiNet kept loaded (prevents UI freeze on next tap)");
    }

    // Original code (now disabled to prevent freeze):
    // multinet_iface_->destroy(multinet_model_);
    // multinet_model_ = nullptr;
    // ESP_LOGI(TAG, "MultiNet unloaded, memory freed for display");
}

void AfeWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                return;
            }
            
            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            
            // Encode all PCM data
            int packets = 0;
            std::vector<int16_t> in_buffer;
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            
            for (auto& pcm: this_->wake_word_pcm_) {
                if (in_buffer.empty()) {
                    in_buffer = std::move(pcm);
                } else {
                    in_buffer.reserve(in_buffer.size() + pcm.size());
                    in_buffer.insert(in_buffer.end(), pcm.begin(), pcm.end());
                }
                
                while (in_buffer.size() >= frame_size) {
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = (uint8_t *)(in_buffer.data());
                    in.len = (uint32_t)(frame_size * sizeof(int16_t));
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;
                    
                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                    }
                    
                    in_buffer.erase(in_buffer.begin(), in_buffer.begin() + frame_size);
                }
            }
            this_->wake_word_pcm_.clear();
            // Close encoder
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
