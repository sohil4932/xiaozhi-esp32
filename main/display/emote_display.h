#pragma once

#include "display.h"
#include <memory>
#include <string>
#include <cstdint>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "expression_emote.h"

namespace emote {

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width, int height);
    virtual ~EmoteDisplay();

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void ShowMicIcon(bool show) override;
    virtual void ShowSpeakerIcon(bool show) override;
    virtual void SetPreviewImage(const void* image);

    bool StopAnimDialog();
    bool InsertAnimDialog(const char* emoji_name, uint32_t duration_ms);

    void RefreshAll();

    // Get emote handle for internal use
    emote_handle_t GetEmoteHandle() const { return emote_handle_; }

private:
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    bool EnsureAssetsReady();
    void ApplyStatusIconState();
    void HideStatusIconOnly();
    void SetStatusIconCentered(bool centered);
    void StopListenAnimation();

    emote_handle_t emote_handle_ = nullptr;
    bool assets_ready_ = false;
    bool mic_icon_visible_ = false;
    bool speaker_icon_visible_ = false;
    bool status_icon_centered_ = false;
    bool status_icon_pos_cached_ = false;
    int16_t status_icon_default_x_ = 0;
    int16_t status_icon_default_y_ = 0;

};

} // namespace emote
