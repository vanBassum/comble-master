#include "BoardContext.h"
#include "esp_log.h"

void BoardContext::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    // A panel that fails to come up must not take the device with it: the web
    // server, the console and OTA are all still worth having on a board whose
    // screen is dead, and that is the difference between a bricked unit and a
    // diagnosable one.
    displayOk_ = display_.Init();
    if (!displayOk_)
        ESP_LOGE(TAG, "Display init failed — continuing headless");

    // Touch is its own question. A panel that lights but cannot be tapped still
    // shows status, and the web UI remains the way in — so this is a warning,
    // not a failure.
    touchOk_ = touch_.Init();
    if (!touchOk_)
        ESP_LOGW(TAG, "Touch init failed — display will be read-only");

    init.SetReady();
    ESP_LOGI(TAG, "Initialized");
}
