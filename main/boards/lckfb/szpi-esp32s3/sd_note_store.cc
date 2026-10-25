#include "sd_note_store.h"

#include "config.h"
#include "note_markdown.h"

#include <driver/sdmmc_host.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#define TAG "SdNoteStore"

namespace {

constexpr char kInspirationDirectory[] = SD_CARD_NOTES_ROOT "/inspiration";
constexpr char kInsightDirectory[] = SD_CARD_NOTES_ROOT "/insight";
constexpr char kReadmePath[] = SD_CARD_NOTES_ROOT "/README.md";

constexpr char kReadme[] = R"(# 小智语音笔记

这里保存通过小智语音记录的内容，按类型和日期分类。

- `inspiration/`：灵感胶囊
- `insight/`：洞察抽屉

每个 Markdown 文件都带有 YAML front matter，包含标题、创建时间和标签。
文件名使用本地时间，格式为 `YYYYMMDD-HHMMSS-mmm.md`。

> 请先插入 FAT32 格式的 SD 卡，再给设备上电，否则记录工具不会出现在工具列表中。
)";

bool WriteTextFile(const char* path, const std::string& content) {
    FILE* file = fopen(path, "w");
    if (file == nullptr) {
        ESP_LOGE(TAG, "Failed to open %s: %s", path, strerror(errno));
        return false;
    }

    const size_t written = fwrite(content.data(), 1, content.size(), file);
    const bool close_ok = fclose(file) == 0;
    if (written != content.size() || !close_ok) {
        ESP_LOGE(TAG, "Failed to write %s", path);
        remove(path);
        return false;
    }
    return true;
}

}  // namespace

SdNoteStore::~SdNoteStore() {
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, card_);
        card_ = nullptr;
    }
}

bool SdNoteStore::Mount() {
    if (card_ != nullptr) {
        return true;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CARD_CLK_PIN;
    slot.cmd = SD_CARD_CMD_PIN;
    slot.d0 = SD_CARD_D0_PIN;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.d4 = GPIO_NUM_NC;
    slot.d5 = GPIO_NUM_NC;
    slot.d6 = GPIO_NUM_NC;
    slot.d7 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t* card = nullptr;
    esp_err_t ret =
        esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return false;
    }

    card_ = card;
    ESP_LOGI(TAG, "SD card mounted at %s", SD_CARD_MOUNT_POINT);

    EnsureDirectory(SD_CARD_NOTES_ROOT);
    EnsureDirectory(kInspirationDirectory);
    EnsureDirectory(kInsightDirectory);
    EnsureReadme();
    return true;
}

bool SdNoteStore::EnsureDirectory(const std::string& path) {
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "Failed to create %s: %s", path.c_str(), strerror(errno));
    return false;
}

void SdNoteStore::EnsureReadme() {
    FILE* file = fopen(kReadmePath, "r");
    if (file != nullptr) {
        fclose(file);
        return;
    }
    if (!WriteTextFile(kReadmePath, kReadme)) {
        ESP_LOGW(TAG, "Could not create the notes README");
    }
}

std::expected<std::string, std::string> SdNoteStore::SaveNote(const std::string& type,
                                                              const std::string& display_name,
                                                              const std::string& title,
                                                              const std::string& content,
                                                              const std::string& tags) {
    if (card_ == nullptr) {
        return std::unexpected("SD card is not mounted; insert a FAT32 card and reboot");
    }
    if (type != "inspiration" && type != "insight") {
        return std::unexpected("invalid note type");
    }
    if (note_markdown::Trim(content).empty()) {
        return std::unexpected("content must not be empty");
    }

    const std::string directory = SD_CARD_NOTES_ROOT "/" + type;
    if (!EnsureDirectory(directory)) {
        return std::unexpected("failed to create the note directory");
    }

    time_t now = time(nullptr);
    struct tm local_time = {};
    localtime_r(&now, &local_time);

    char date[16] = {};
    char timestamp[32] = {};
    char created_at[40] = {};
    strftime(date, sizeof(date), "%Y-%m-%d", &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &local_time);
    strftime(created_at, sizeof(created_at), "%Y-%m-%dT%H:%M:%S", &local_time);

    if (now < 1700000000) {
        ESP_LOGW(TAG, "System time is not synchronized; using %s for the note date", date);
    }

    const std::string day_directory = directory + "/" + date;
    if (!EnsureDirectory(day_directory)) {
        return std::unexpected("failed to create the date directory");
    }

    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    char path[160] = {};
    snprintf(path, sizeof(path), "%s/%s-%03u.md", day_directory.c_str(), timestamp,
             static_cast<unsigned>(tv.tv_usec / 1000));

    const std::string normalized_title =
        note_markdown::NormalizeTitle(title, content, "未命名笔记");
    const std::string markdown = note_markdown::BuildMarkdown(type, display_name, normalized_title,
                                                              content, tags, created_at);

    if (!WriteTextFile(path, markdown)) {
        return std::unexpected(std::string("failed to write the note: ") + path);
    }

    ESP_LOGI(TAG, "Saved %s note to %s", type.c_str(), path);
    return std::string(path);
}
