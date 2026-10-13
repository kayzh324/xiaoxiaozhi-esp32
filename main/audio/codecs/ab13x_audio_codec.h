#ifndef _AB13X_AUDIO_CODEC_H
#define _AB13X_AUDIO_CODEC_H

#include "audio_codec.h"

#include <usb/uac_host.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <atomic>
#include <mutex>
#include <vector>

// USB UAC 1.0 audio codec for headsets built on the AB13X (and other UAC 1.0)
// adapter chips. The device is presented to the host as two independent logic
// devices: a TX (speaker) interface and an RX (microphone) interface, so both
// are opened and driven separately.
class Ab13xAudioCodec : public AudioCodec {
public:
    Ab13xAudioCodec();
    ~Ab13xAudioCodec() override;

    void SetOutputVolume(int volume) override;

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

private:
    // Negotiated stream format of one UAC interface (speaker or microphone).
    struct StreamFormat {
        uint8_t channels = 1;
        uint8_t subframe_size = 2;
        uint8_t bit_resolution = 16;
        uint32_t sample_freq = 48000;
        bool valid = false;
    };

    enum class EventKind : uint8_t {
        Driver,
        Device,
    };

    struct EventRecord {
        EventKind kind;
        union {
            struct {
                uint8_t addr;
                uint8_t iface_num;
                uac_host_driver_event_t event;
            } driver;
            struct {
                uac_host_device_handle_t handle;
                uac_host_device_event_t event;
            } device;
        };
    };

    void StartUsbHost();
    static void UsbHostTaskEntry(void* arg);
    void UsbHostTask();
    void LogStatus();
    static void EventTaskEntry(void* arg);
    void EventTask();

    static void DriverEventCallback(uint8_t addr, uint8_t iface_num, uac_host_driver_event_t event,
                                    void* arg);
    static void DeviceEventCallback(uac_host_device_handle_t handle, uac_host_device_event_t event,
                                    void* arg);

    void OnDriverEvent(uint8_t addr, uint8_t iface_num, uac_host_driver_event_t event);
    void OnDeviceEvent(uac_host_device_handle_t handle, uac_host_device_event_t event);

    void OpenSpeaker(uint8_t addr, uint8_t iface_num);
    void OpenMicrophone(uint8_t addr, uint8_t iface_num);
    void CloseInterface(uac_host_device_handle_t handle);
    void ReArmMicrophone();

    // Scan the interface's alternate settings (starting at 1, not 0) for one
    // that supports the requested frequency and bit depth.
    static bool FindAlternateForFormat(uac_host_device_handle_t handle, uint32_t sample_freq,
                                       uint8_t bit_resolution, StreamFormat* out);
    static StreamFormat PickFirstAlternate(uac_host_device_handle_t handle);
    static void LogAlternateParams(uac_host_device_handle_t handle, const char* label);

    static void MixToMono(const uint8_t* src, int frames, int channels, int subframe_size,
                          int16_t* dest);
    static void ExpandFromMono(const int16_t* src, int frames, int channels, int subframe_size,
                               uint8_t* dest);

    QueueHandle_t event_queue_ = nullptr;
    TaskHandle_t event_task_ = nullptr;
    TaskHandle_t usb_task_ = nullptr;
    std::mutex handle_mutex_;

    uac_host_device_handle_t spk_handle_ = nullptr;
    uac_host_device_handle_t mic_handle_ = nullptr;
    StreamFormat spk_format_;
    StreamFormat mic_format_;

    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;

    std::atomic<bool> hw_volume_supported_{false};
    std::atomic<uint32_t> read_bytes_{0};
    std::atomic<uint32_t> write_bytes_{0};
    // Diagnostics: read_calls_ tells whether AudioService asks for capture at all,
    // rx_done_events_ tells whether the UAC driver sees inbound packets, and
    // mic_peak_/mic_abs_sum_/mic_abs_count_ separate "streaming but silent" from
    // "streaming real audio". The peak holds the maximum since the last status log;
    // the mean of |sample| is robust to a single transient and is the better tuning
    // signal. All three are cleared when reported.
    std::atomic<uint32_t> read_calls_{0};
    std::atomic<uint32_t> rx_done_events_{0};
    std::atomic<int32_t> mic_peak_{0};
    std::atomic<int64_t> mic_abs_sum_{0};
    std::atomic<uint32_t> mic_abs_count_{0};
    // Capture gain the device accepted, in 1/256 dB (0 when it refused any).
    std::atomic<int16_t> mic_hw_gain_db_{0};
    std::atomic<uint32_t> mic_rearms_{0};
    // Watchdog state, only touched from the UAC event task.
    uint32_t last_mic_events_ = 0;
    uint32_t last_read_bytes_ = 0;
    int mic_idle_intervals_ = 0;
};

#endif  // _AB13X_AUDIO_CODEC_H
