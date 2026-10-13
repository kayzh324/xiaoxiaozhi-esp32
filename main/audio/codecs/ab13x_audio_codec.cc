#include "ab13x_audio_codec.h"

#include <esp_log.h>
#include <usb/usb_host.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#define TAG "Ab13xAudioCodec"

#define USB_HOST_TASK_PRIORITY 5
#define EVENT_TASK_PRIORITY 5
#define EVENT_QUEUE_LENGTH 10
#define EVENT_TASK_STACK_SIZE 4096
#define USB_HOST_TASK_STACK 4096
#define STATUS_LOG_INTERVAL_MS 5000

// The device side of a UAC 1.0 headset adapter typically only offers 48 kHz.
// We declare 48 kHz here; AudioService resamples between this and the 16 kHz
// pipeline in both directions, so the codec itself does no rate conversion.
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_BIT_DEPTH 16

// Target total capture gain in dB, analog plus software. The AB13X's own capture gain
// is already at its ceiling: set_volume(100) makes it report 8191 (1/256 dB), i.e.
// about +32 dB, and it will not go higher. At that maximum, normal-distance speech
// still only peaks at 1145 (-29 dBFS) over a noise floor near -44 dBFS, so it needs
// roughly 15-17 dB more - and only software can supply it, because the analog path is
// exhausted. Software gain cannot change the signal-to-noise ratio, only the level the
// ASR/Opus front end sees; it is worth having because both lose effective resolution
// on a signal this quiet.
#define MIC_TOTAL_GAIN_DB 46.0f
// Ceiling on the software share. At the measured +32 dB of analog gain, 14 dB of
// software gain puts a normal-distance peak at 1145 * 5.0 = 5725 (-15 dBFS) and
// close-talk at 4199 * 5.0 = 20995 (-4 dBFS), both clear of the 32767 rail. This also
// caps the boost when the device reports little or no gain of its own.
#define MIC_MAX_SOFTWARE_GAIN_DB 20.0f
// Status-log intervals the microphone may stay silent before capture is re-armed.
#define MIC_REARM_IDLE_INTERVALS 2

Ab13xAudioCodec::Ab13xAudioCodec() {
    duplex_ = true;            // Speaker and microphone share the bus.
    input_reference_ = false;  // Headset has no echo-reference channel.
    input_channels_ = 1;       // Downmix the capture stream to mono.
    output_channels_ = 1;
    input_sample_rate_ = DEFAULT_SAMPLE_RATE;
    output_sample_rate_ = DEFAULT_SAMPLE_RATE;
    // input_gain_ is set from the device's accepted analog gain in OpenMicrophone().

    event_queue_ = xQueueCreate(EVENT_QUEUE_LENGTH, sizeof(EventRecord));
    if (event_queue_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return;
    }

    if (xTaskCreate(EventTaskEntry, "uac_events", EVENT_TASK_STACK_SIZE, this, EVENT_TASK_PRIORITY,
                    &event_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UAC event task");
        return;
    }

    if (xTaskCreate(UsbHostTaskEntry, "usb_host", USB_HOST_TASK_STACK, this, USB_HOST_TASK_PRIORITY,
                    &usb_task_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create USB host task");
    }
}

Ab13xAudioCodec::~Ab13xAudioCodec() {
    std::lock_guard<std::mutex> lock(handle_mutex_);
    if (mic_handle_ != nullptr) {
        uac_host_device_close(mic_handle_);
        mic_handle_ = nullptr;
    }
    if (spk_handle_ != nullptr) {
        uac_host_device_close(spk_handle_);
        spk_handle_ = nullptr;
    }
    uac_host_uninstall();
}

// ---------------------------------------------------------------------------
// USB host bring-up
// ---------------------------------------------------------------------------

void Ab13xAudioCodec::UsbHostTaskEntry(void* arg) {
    static_cast<Ab13xAudioCodec*>(arg)->UsbHostTask();
    vTaskDelete(nullptr);
}

void Ab13xAudioCodec::UsbHostTask() {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.root_port_unpowered = false;
    host_config.intr_flags = ESP_INTR_FLAG_LOWMED;
    // The AB13X advertises a 208-byte isochronous IN MPS and a 384-byte
    // isochronous OUT MPS. No stock FIFO bias fits both: Balanced starves the
    // periodic OUT FIFO (32 lines = 128 B < 384 B) and Periodic OUT starves the
    // RX FIFO (34 lines = 136 B < 208 B). So split the controller's budget by
    // hand. The budget is GHWCFG3.dfifodepth = OTG_DFIFO_DEPTH (256) - EP_LOC_CNT
    // (56) = 200 lines, 4 bytes per line. rx + nptx + ptx must not exceed it:
    // usb_dwc_hal_fifo_config_is_valid() checks exactly that when the port is
    // reset, and an over-budget config aborts the reset ("Invalid FIFO config")
    // so the device never enumerates.
    host_config.fifo_settings_custom.rx_fifo_lines = 56;    // 224 B: mic MPS 208 + 2 status lines
    host_config.fifo_settings_custom.ptx_fifo_lines = 104;  // 416 B: speaker MPS 384
    host_config.fifo_settings_custom.nptx_fifo_lines = 32;  // 128 B: control/bulk OUT
    // Total 192 of 200 lines.
    esp_err_t host_ret = usb_host_install(&host_config);
    if (host_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(host_ret));
        return;
    }
    ESP_LOGI(TAG, "USB host installed (FIFO lines rx=%u ptx=%u nptx=%u)",
             (unsigned)host_config.fifo_settings_custom.rx_fifo_lines,
             (unsigned)host_config.fifo_settings_custom.ptx_fifo_lines,
             (unsigned)host_config.fifo_settings_custom.nptx_fifo_lines);

    // The UAC driver runs its own background task for client events, but the
    // library-level events (device enumeration) still need a drain loop here.
    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = EVENT_TASK_PRIORITY,
        .stack_size = EVENT_TASK_STACK_SIZE,
        .core_id = tskNO_AFFINITY,
        .callback = DriverEventCallback,
        .callback_arg = this,
    };
    esp_err_t uac_ret = uac_host_install(&uac_config);
    if (uac_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UAC driver: %s", esp_err_to_name(uac_ret));
        return;
    }
    ESP_LOGI(TAG, "UAC driver installed, waiting for headset");

    while (true) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

void Ab13xAudioCodec::LogStatus() {
    uac_host_device_handle_t spk_handle;
    uac_host_device_handle_t mic_handle;
    StreamFormat mic_format;
    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        spk_handle = spk_handle_;
        mic_handle = mic_handle_;
        mic_format = mic_format_;
    }
    if (spk_handle != nullptr && spk_handle == mic_handle) {
        ESP_LOGE(TAG,
                 "Speaker and microphone share one UAC handle - the capture direction is not "
                 "actually streaming");
    }
    const int64_t mic_abs_sum = mic_abs_sum_.exchange(0);
    const uint32_t mic_abs_count = mic_abs_count_.exchange(0);
    const int mic_avg = mic_abs_count > 0 ? static_cast<int>(mic_abs_sum / mic_abs_count) : 0;
    // The negotiated capture format is repeated here because swapping in a different
    // USB adapter changes the descriptors, and both the hardcoded 48000 Hz and the FIFO
    // split were tuned for this one. Kept at debug level: the call itself must still
    // happen every STATUS_LOG_INTERVAL_MS because it drives the capture watchdog below.
    ESP_LOGD(TAG,
             "status: speaker=%s mic=%s mic_fmt=%uHz/%uch/%ubit hw_volume=%d rx=%uB tx=%uB "
             "read_calls=%u mic_events=%u mic_peak=%d mic_avg=%d mic_hw_gain=%d mic_rearms=%u",
             spk_handle != nullptr ? "up" : "down", mic_handle != nullptr ? "up" : "down",
             (unsigned)mic_format.sample_freq, mic_format.channels, mic_format.bit_resolution,
             hw_volume_supported_.load() ? 1 : 0, (unsigned)read_bytes_.load(),
             (unsigned)write_bytes_.load(), (unsigned)read_calls_.load(),
             (unsigned)rx_done_events_.load(), (int)mic_peak_.exchange(0), mic_avg,
             (int)mic_hw_gain_db_.load(), (unsigned)mic_rearms_.load());

    // Capture watchdog. Liveness has to be judged from BOTH counters: RX_DONE only
    // fires once inbound packets cross the ringbuffer threshold, so while Read() is
    // actively draining the buffer it stops firing even though the device is
    // streaming happily. read_bytes_ covers that case; RX_DONE covers the idle case
    // where nobody drains the buffer. Only when neither advances has the device
    // really stopped sending.
    const uint32_t mic_events = rx_done_events_.load();
    const uint32_t read_bytes = read_bytes_.load();
    if (mic_handle != nullptr && mic_events == last_mic_events_ && read_bytes == last_read_bytes_) {
        if (++mic_idle_intervals_ >= MIC_REARM_IDLE_INTERVALS) {
            mic_idle_intervals_ = 0;
            ReArmMicrophone();
        }
    } else {
        mic_idle_intervals_ = 0;
    }
    last_mic_events_ = mic_events;
    last_read_bytes_ = read_bytes;
}

void Ab13xAudioCodec::ReArmMicrophone() {
    uac_host_device_handle_t handle;
    StreamFormat format;
    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        handle = mic_handle_;
        format = mic_format_;
    }
    if (handle == nullptr) {
        return;
    }

    // A UAC alternate setting only takes effect on a real 0 -> N transition. The
    // AB13X leaves its capture engine idle when the host claims alt N directly,
    // which is what happens whenever the ESP32 reboots while the headset stays
    // powered (a USB bus reset does not power-cycle the adapter). Suspend drops
    // the interface to alt 0, resume re-selects alt N and resubmits the IN URBs.
    mic_rearms_++;
    ESP_LOGW(TAG, "No microphone packets for %d s, re-arming capture (attempt %u)",
             STATUS_LOG_INTERVAL_MS * MIC_REARM_IDLE_INTERVALS / 1000,
             (unsigned)mic_rearms_.load());

    esp_err_t ret = uac_host_device_suspend(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Microphone suspend failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = uac_host_device_resume(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Microphone resume failed: %s", esp_err_to_name(ret));
    }
}

// ---------------------------------------------------------------------------
// Event plumbing
// ---------------------------------------------------------------------------

void Ab13xAudioCodec::DriverEventCallback(uint8_t addr, uint8_t iface_num,
                                          uac_host_driver_event_t event, void* arg) {
    auto* self = static_cast<Ab13xAudioCodec*>(arg);
    EventRecord record = {};
    record.kind = EventKind::Driver;
    record.driver.addr = addr;
    record.driver.iface_num = iface_num;
    record.driver.event = event;
    // Runs in the USB task context; must not block.
    if (xQueueSend(self->event_queue_, &record, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Driver event queue full");
    }
}

void Ab13xAudioCodec::DeviceEventCallback(uac_host_device_handle_t handle,
                                          uac_host_device_event_t event, void* arg) {
    auto* self = static_cast<Ab13xAudioCodec*>(arg);
    EventRecord record = {};
    record.kind = EventKind::Device;
    record.device.handle = handle;
    record.device.event = event;
    if (xQueueSend(self->event_queue_, &record, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Device event queue full");
    }
}

void Ab13xAudioCodec::EventTaskEntry(void* arg) {
    static_cast<Ab13xAudioCodec*>(arg)->EventTask();
    vTaskDelete(nullptr);
}

void Ab13xAudioCodec::EventTask() {
    while (true) {
        EventRecord record = {};
        // The timeout doubles as a heartbeat: with no USB events flowing it lets us
        // report whether the endpoints are open and whether audio bytes move at all.
        if (xQueueReceive(event_queue_, &record, pdMS_TO_TICKS(STATUS_LOG_INTERVAL_MS)) != pdTRUE) {
            LogStatus();
            continue;
        }
        if (record.kind == EventKind::Driver) {
            OnDriverEvent(record.driver.addr, record.driver.iface_num, record.driver.event);
        } else {
            OnDeviceEvent(record.device.handle, record.device.event);
        }
    }
}

void Ab13xAudioCodec::OnDriverEvent(uint8_t addr, uint8_t iface_num,
                                    uac_host_driver_event_t event) {
    switch (event) {
        case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
            ESP_LOGI(TAG, "Speaker interface %u connected", iface_num);
            OpenSpeaker(addr, iface_num);
            break;
        case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
            ESP_LOGI(TAG, "Microphone interface %u connected", iface_num);
            OpenMicrophone(addr, iface_num);
            break;
        default:
            break;
    }
}

void Ab13xAudioCodec::OnDeviceEvent(uac_host_device_handle_t handle,
                                    uac_host_device_event_t event) {
    switch (event) {
        case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "UAC interface disconnected");
            CloseInterface(handle);
            break;
        case UAC_HOST_DEVICE_EVENT_RX_DONE:
            rx_done_events_++;
            break;
        case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "UAC transfer error");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Interface open / format negotiation
// ---------------------------------------------------------------------------

void Ab13xAudioCodec::OpenSpeaker(uint8_t addr, uint8_t iface_num) {
    const uac_host_device_config_t dev_config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = 16384,
        .buffer_threshold = 4096,
        .callback = DeviceEventCallback,
        .callback_arg = this,
    };

    uac_host_device_handle_t handle = nullptr;
    esp_err_t open_ret = uac_host_device_open(&dev_config, &handle);
    if (open_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open speaker interface (addr=%u iface=%u): %s", addr, iface_num,
                 esp_err_to_name(open_ret));
        return;
    }
    LogAlternateParams(handle, "speaker");
    uac_host_dev_info_t spk_info = {};
    if (uac_host_get_device_info(handle, &spk_info) == ESP_OK && spk_info.type != UAC_STREAM_TX) {
        ESP_LOGE(TAG, "Speaker handle reports stream type %d, expected Speaker (TX)",
                 spk_info.type);
    }

    StreamFormat format;
    if (!FindAlternateForFormat(handle, DEFAULT_SAMPLE_RATE, DEFAULT_BIT_DEPTH, &format)) {
        ESP_LOGW(TAG, "Speaker has no %u Hz / %u bit alternate, falling back", DEFAULT_SAMPLE_RATE,
                 DEFAULT_BIT_DEPTH);
        format = PickFirstAlternate(handle);
    }
    if (!format.valid) {
        ESP_LOGE(TAG, "Speaker has no usable alternate setting");
        uac_host_device_close(handle);
        return;
    }

    const uac_host_stream_config_t stream_config = {
        .channels = format.channels,
        .bit_resolution = format.bit_resolution,
        .sample_freq = format.sample_freq,
        .flags = 0,
    };
    esp_err_t start_ret = uac_host_device_start(handle, &stream_config);
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start speaker stream (%u Hz, %u ch, %u bit): %s",
                 format.sample_freq, format.channels, format.bit_resolution,
                 esp_err_to_name(start_ret));
        uac_host_device_close(handle);
        return;
    }
    ESP_LOGI(TAG, "Speaker started: %u Hz, %u ch, %u bit", format.sample_freq, format.channels,
             format.bit_resolution);

    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        spk_handle_ = handle;
        spk_format_ = format;
    }

    // The AB13X adapter usually exposes no feature unit; remember whether the
    // hardware honours volume so SetOutputVolume can fall back to scaling.
    // Unmute regardless: many UAC adapters boot muted.
    esp_err_t volume_err = uac_host_device_set_volume(handle, output_volume_);
    hw_volume_supported_.store(volume_err == ESP_OK);
    if (uac_host_device_set_mute(handle, false) != ESP_OK) {
        ESP_LOGW(TAG, "Speaker mute control unsupported");
    }
    if (volume_err != ESP_OK) {
        ESP_LOGW(TAG, "Hardware volume unsupported, using software volume");
    }
}

void Ab13xAudioCodec::OpenMicrophone(uint8_t addr, uint8_t iface_num) {
    const uac_host_device_config_t dev_config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = 16384,
        .buffer_threshold = 4096,
        .callback = DeviceEventCallback,
        .callback_arg = this,
    };

    uac_host_device_handle_t handle = nullptr;
    esp_err_t open_ret = uac_host_device_open(&dev_config, &handle);
    if (open_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open microphone interface (addr=%u iface=%u): %s", addr, iface_num,
                 esp_err_to_name(open_ret));
        return;
    }
    LogAlternateParams(handle, "microphone");
    uac_host_dev_info_t mic_info = {};
    if (uac_host_get_device_info(handle, &mic_info) == ESP_OK && mic_info.type != UAC_STREAM_RX) {
        ESP_LOGE(TAG, "Microphone handle reports stream type %d, expected Microphone (RX)",
                 mic_info.type);
    }

    StreamFormat format;
    if (!FindAlternateForFormat(handle, DEFAULT_SAMPLE_RATE, DEFAULT_BIT_DEPTH, &format)) {
        ESP_LOGW(TAG, "Microphone has no %u Hz / %u bit alternate, falling back",
                 DEFAULT_SAMPLE_RATE, DEFAULT_BIT_DEPTH);
        format = PickFirstAlternate(handle);
    }
    if (!format.valid) {
        ESP_LOGE(TAG, "Microphone has no usable alternate setting");
        uac_host_device_close(handle);
        return;
    }

    const uac_host_stream_config_t stream_config = {
        .channels = format.channels,
        .bit_resolution = format.bit_resolution,
        .sample_freq = format.sample_freq,
        .flags = 0,
    };
    esp_err_t start_ret = uac_host_device_start(handle, &stream_config);
    if (start_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start microphone stream (%u Hz, %u ch, %u bit): %s",
                 format.sample_freq, format.channels, format.bit_resolution,
                 esp_err_to_name(start_ret));
        uac_host_device_close(handle);
        return;
    }

    ESP_LOGI(TAG, "Microphone started: %u Hz, %u ch, %u bit", format.sample_freq, format.channels,
             format.bit_resolution);

    // UAC adapters commonly boot muted with zero gain, which yields silent
    // capture even though the stream runs; unmute first.
    if (uac_host_device_set_mute(handle, false) != ESP_OK) {
        ESP_LOGW(TAG, "Microphone mute control unsupported");
    }

    // Push the device's own capture gain to its ceiling first. This adapter reports
    // ~+32 dB at set_volume(100) and that analog gain - ahead of the ADC - is the only
    // kind that can help the signal-to-noise ratio, so take all of it before adding any
    // software gain. Requesting a fixed dB here would be wrong: the ceiling is above
    // 30 dB, so asking for less would quietly LOWER the capture level.
    int16_t hw_gain_db = 0;
    if (uac_host_device_set_volume(handle, 100) != ESP_OK) {
        ESP_LOGW(TAG, "Microphone hardware gain unsupported");
    } else if (uac_host_device_get_volume_db(handle, &hw_gain_db) != ESP_OK) {
        ESP_LOGW(TAG, "Microphone capture gain unreadable");
    }
    mic_hw_gain_db_.store(hw_gain_db);
    input_gain_ =
        std::clamp(MIC_TOTAL_GAIN_DB - hw_gain_db / 256.0f, 0.0f, MIC_MAX_SOFTWARE_GAIN_DB);
    ESP_LOGI(TAG, "Microphone capture gain: %d (1/256 dB) at the device, %.1f dB in software",
             hw_gain_db, input_gain_);

    std::lock_guard<std::mutex> lock(handle_mutex_);
    mic_handle_ = handle;
    mic_format_ = format;
}

void Ab13xAudioCodec::CloseInterface(uac_host_device_handle_t handle) {
    std::lock_guard<std::mutex> lock(handle_mutex_);
    if (handle == spk_handle_) {
        spk_handle_ = nullptr;
        spk_format_ = {};
    } else if (handle == mic_handle_) {
        mic_handle_ = nullptr;
        mic_format_ = {};
    }
    uac_host_device_close(handle);
}

bool Ab13xAudioCodec::FindAlternateForFormat(uac_host_device_handle_t handle, uint32_t sample_freq,
                                             uint8_t bit_resolution, StreamFormat* out) {
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(handle, &info) != ESP_OK) {
        return false;
    }

    // Alternate settings are 1-based; alternate 0 is the zero-bandwidth idle
    // setting. Walk every advertised alternate and take the first that matches.
    for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t param = {};
        if (uac_host_get_device_alt_param(handle, alt, &param) != ESP_OK) {
            continue;
        }
        if (param.bit_resolution != bit_resolution) {
            continue;
        }

        bool freq_match = false;
        if (param.sample_freq_type > 0) {
            for (int i = 0; i < param.sample_freq_type; i++) {
                if (param.sample_freq[i] == sample_freq) {
                    freq_match = true;
                    break;
                }
            }
        } else {
            freq_match =
                param.sample_freq_lower <= sample_freq && param.sample_freq_upper >= sample_freq;
        }
        if (!freq_match) {
            continue;
        }

        out->channels = param.channels;
        out->subframe_size = param.subframe_size;
        out->bit_resolution = param.bit_resolution;
        out->sample_freq = sample_freq;
        out->valid = true;
        return true;
    }
    return false;
}

void Ab13xAudioCodec::LogAlternateParams(uac_host_device_handle_t handle, const char* label) {
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(handle, &info) != ESP_OK) {
        ESP_LOGW(TAG, "%s: failed to read device info", label);
        return;
    }
    ESP_LOGI(TAG, "%s: type=%s iface=%u addr=%u vid=0x%04X pid=0x%04X alts=%u", label,
             info.type == UAC_STREAM_TX ? "Speaker" : "Microphone", info.iface_num, info.addr,
             info.VID, info.PID, info.iface_alt_num);
    for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t param = {};
        if (uac_host_get_device_alt_param(handle, alt, &param) != ESP_OK) {
            ESP_LOGW(TAG, "%s alt %u: unreadable", label, alt);
            continue;
        }
        if (param.sample_freq_type > 0) {
            char freqs[64] = {};
            size_t written = 0;
            for (int i = 0; i < param.sample_freq_type && written + 12 < sizeof(freqs); i++) {
                written += snprintf(freqs + written, sizeof(freqs) - written, "%s%u",
                                    i == 0 ? "" : ",", (unsigned)param.sample_freq[i]);
            }
            ESP_LOGI(TAG, "%s alt %u: ch=%u subframe=%u bit=%u freq=[%s]", label, alt,
                     param.channels, param.subframe_size, param.bit_resolution, freqs);
        } else {
            ESP_LOGI(TAG, "%s alt %u: ch=%u subframe=%u bit=%u freq=%u..%u", label, alt,
                     param.channels, param.subframe_size, param.bit_resolution,
                     (unsigned)param.sample_freq_lower, (unsigned)param.sample_freq_upper);
        }
    }
}

Ab13xAudioCodec::StreamFormat Ab13xAudioCodec::PickFirstAlternate(uac_host_device_handle_t handle) {
    StreamFormat format;
    uac_host_dev_info_t info = {};
    if (uac_host_get_device_info(handle, &info) != ESP_OK) {
        return format;
    }

    uac_host_dev_alt_param_t param = {};
    if (uac_host_get_device_alt_param(handle, 1, &param) != ESP_OK) {
        return format;
    }

    uint32_t freq = 0;
    if (param.sample_freq_type > 0) {
        freq = param.sample_freq[0];
    } else {
        freq = param.sample_freq_lower != 0 ? param.sample_freq_lower : param.sample_freq_upper;
    }

    format.channels = param.channels;
    format.subframe_size = param.subframe_size;
    format.bit_resolution = param.bit_resolution;
    format.sample_freq = freq;
    format.valid = freq != 0;
    ESP_LOGW(TAG, "Falling back to alternate 1: %u Hz, %u ch, %u bit", format.sample_freq,
             format.channels, format.bit_resolution);
    return format;
}

// ---------------------------------------------------------------------------
// Sample format conversion
// ---------------------------------------------------------------------------

void Ab13xAudioCodec::MixToMono(const uint8_t* src, int frames, int channels, int subframe_size,
                                int16_t* dest) {
    for (int frame = 0; frame < frames; frame++) {
        int32_t sum = 0;
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t* sample = src + (frame * channels + ch) * subframe_size;
            // Take the high 16 bits of the subframe; for 16-bit PCM this is the
            // value itself, for wider subframes it doubles as a truncation.
            int16_t value;
            if (subframe_size >= 3) {
                value = static_cast<int16_t>((sample[2] << 8) | sample[1]);
            } else {
                value = static_cast<int16_t>((sample[1] << 8) | sample[0]);
            }
            sum += value;
        }
        dest[frame] = static_cast<int16_t>(sum / channels);
    }
}

void Ab13xAudioCodec::ExpandFromMono(const int16_t* src, int frames, int channels,
                                     int subframe_size, uint8_t* dest) {
    for (int frame = 0; frame < frames; frame++) {
        int16_t value = src[frame];
        for (int ch = 0; ch < channels; ch++) {
            uint8_t* sample = dest + (frame * channels + ch) * subframe_size;
            if (subframe_size >= 3) {
                sample[0] = 0;
                sample[1] = value & 0xFF;
                sample[2] = (value >> 8) & 0xFF;
            } else {
                sample[0] = value & 0xFF;
                sample[1] = (value >> 8) & 0xFF;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AudioCodec interface
// ---------------------------------------------------------------------------

int Ab13xAudioCodec::Read(int16_t* dest, int samples) {
    read_calls_++;
    // No device yet: report no samples. AudioService treats this as "no data".
    uac_host_device_handle_t handle;
    StreamFormat format;
    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        handle = mic_handle_;
        format = mic_format_;
    }
    if (handle == nullptr || samples <= 0) {
        return 0;
    }

    const int channels = format.channels > 0 ? format.channels : 1;
    const int subframe = format.subframe_size > 0 ? format.subframe_size : 2;
    const int frame_size = channels * subframe;
    const int want_bytes = samples * frame_size;

    if (static_cast<int>(read_buffer_.size()) < want_bytes) {
        read_buffer_.resize(want_bytes);
    }

    int filled = 0;
    // Accumulate whole frames until the request is satisfied or we time out.
    while (filled < want_bytes) {
        uint32_t bytes_read = 0;
        esp_err_t ret = uac_host_device_read(handle, read_buffer_.data() + filled,
                                             want_bytes - filled, &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGD(TAG, "Microphone read failed: %s", esp_err_to_name(ret));
            break;
        }
        if (bytes_read == 0) {
            break;
        }
        filled += bytes_read;
    }

    const int frames = filled / frame_size;
    read_bytes_ += filled;
    if (frames > 0) {
        MixToMono(read_buffer_.data(), frames, channels, subframe, dest);
    }
    if (frames < samples) {
        std::memset(dest + frames, 0, (samples - frames) * sizeof(int16_t));
    }

    if (input_gain_ != 0.0f) {
        const float gain = std::pow(10.0f, input_gain_ / 20.0f);
        for (int i = 0; i < samples; i++) {
            dest[i] = static_cast<int16_t>(std::clamp(dest[i] * gain, -32768.0f, 32767.0f));
        }
    }

    int32_t peak = 0;
    int64_t abs_sum = 0;
    for (int i = 0; i < samples; i++) {
        const int32_t magnitude = dest[i] < 0 ? -static_cast<int32_t>(dest[i]) : dest[i];
        if (magnitude > peak) {
            peak = magnitude;
        }
        abs_sum += magnitude;
    }
    // Hold the peak across the status interval: a single Read() only covers ~10 ms of
    // speech, so storing just its peak badly under-samples the envelope. The running
    // |sample| sum alongside it gives a level reading that one transient cannot skew.
    if (peak > mic_peak_.load()) {
        mic_peak_.store(peak);
    }
    mic_abs_sum_.fetch_add(abs_sum);
    mic_abs_count_.fetch_add(static_cast<uint32_t>(samples));
    return samples;
}

int Ab13xAudioCodec::Write(const int16_t* data, int samples) {
    uac_host_device_handle_t handle;
    StreamFormat format;
    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        handle = spk_handle_;
        format = spk_format_;
    }
    if (handle == nullptr || samples <= 0) {
        return samples;  // Consume silently; nothing is listening yet.
    }

    const int channels = format.channels > 0 ? format.channels : 1;
    const int subframe = format.subframe_size > 0 ? format.subframe_size : 2;
    const int want_bytes = samples * channels * subframe;

    if (static_cast<int>(write_buffer_.size()) < want_bytes) {
        write_buffer_.resize(want_bytes);
    }

    std::vector<int16_t> scaled;
    const int16_t* source = data;
    if (!hw_volume_supported_.load()) {
        // Hardware volume is unavailable, so apply it in software.
        scaled.resize(samples);
        const float gain = std::clamp(output_volume_ / 100.0f, 0.0f, 1.0f);
        for (int i = 0; i < samples; i++) {
            scaled[i] = static_cast<int16_t>(source[i] * gain);
        }
        source = scaled.data();
    }

    ExpandFromMono(source, samples, channels, subframe, write_buffer_.data());

    esp_err_t ret =
        uac_host_device_write(handle, write_buffer_.data(), want_bytes, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Speaker write failed: %s", esp_err_to_name(ret));
    } else {
        write_bytes_ += want_bytes;
    }
    return samples;
}

void Ab13xAudioCodec::SetOutputVolume(int volume) {
    uac_host_device_handle_t handle;
    {
        std::lock_guard<std::mutex> lock(handle_mutex_);
        handle = spk_handle_;
    }
    if (handle != nullptr && hw_volume_supported_.load()) {
        uac_host_device_set_volume(handle, volume);
    }
    AudioCodec::SetOutputVolume(volume);
}
