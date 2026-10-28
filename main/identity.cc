#include "identity.h"

#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cstdio>

#define TAG "Identity"

namespace {

constexpr int kDefaultCount = 4;
constexpr int kMaxCount = 4;
constexpr int kMaxNameBytes = 24;

const char* const kDefaultNames[kMaxCount] = {"情绪聊天", "顾问咨询", "思考伙伴", "自定义"};

std::string NameKey(int slot) {
    return "name" + std::to_string(slot);
}

std::string SlotKey(int slot, const char* suffix) {
    char key[16];
    snprintf(key, sizeof(key), "id%d_%s", slot, suffix);
    return key;
}

// Locally administered, unicast MAC: first octet 0x02 (bit1=1, bit0=0).
std::string GenerateDeviceId() {
    uint8_t bytes[5];
    esp_fill_random(bytes, sizeof(bytes));
    char mac[18];
    snprintf(mac, sizeof(mac), "02:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1], bytes[2],
             bytes[3], bytes[4]);
    return mac;
}

std::string GenerateUuid() {
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uuid[6] & 0x0F) | 0x40;  // version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  // variant 1
    char uuid_str[37];
    snprintf(uuid_str, sizeof(uuid_str),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
             uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return uuid_str;
}

/**
 * The pre-profile firmware stored credentials in the plain "mqtt"/"websocket"
 * namespaces, which belong to the real identity. Copy them into the slot-0
 * namespaces once so an existing binding survives the upgrade.
 *
 * Deliberately slot 0 only: copying the legacy credentials into slot 1..N would
 * hand a fresh identity the real device's client_id/username, which would either
 * hijack the old session or silently talk to the wrong account.
 */
void MigrateLegacyCredentials(const std::string& base, const std::string& target) {
    Settings target_settings(target, true);
    Settings legacy(base, false);

    if (base == "mqtt") {
        if (!target_settings.GetString("endpoint").empty()) {
            return;
        }
        if (legacy.GetString("endpoint").empty()) {
            return;
        }
        for (const char* key : {"endpoint", "client_id", "username", "password",
                                "publish_topic", "subscribe_topic"}) {
            auto value = legacy.GetString(key);
            if (!value.empty()) {
                target_settings.SetString(key, value);
            }
        }
        int keepalive = legacy.GetInt("keepalive", 0);
        if (keepalive > 0) {
            target_settings.SetInt("keepalive", keepalive);
        }
    } else {
        if (!target_settings.GetString("url").empty()) {
            return;
        }
        if (legacy.GetString("url").empty()) {
            return;
        }
        for (const char* key : {"url", "token"}) {
            auto value = legacy.GetString(key);
            if (!value.empty()) {
                target_settings.SetString(key, value);
            }
        }
        int version = legacy.GetInt("version", 0);
        if (version > 0) {
            target_settings.SetInt("version", version);
        }
    }
    ESP_LOGI(TAG, "Migrated legacy %s credentials to %s", base.c_str(), target.c_str());
}

}  // namespace

int Identity::GetCount() {
    Settings settings("identity", false);
    int count = settings.GetInt("count", kDefaultCount);
    if (count < 1) {
        count = 1;
    } else if (count > kMaxCount) {
        count = kMaxCount;
    }
    return count;
}

int Identity::GetSlot() {
    Settings settings("identity", false);
    int slot = settings.GetInt("slot", 0);
    if (slot < 0 || slot >= GetCount()) {
        return 0;
    }
    return slot;
}

Identity::Profile Identity::Load(int slot) {
    Profile profile{slot, "", "", false};
    if (slot == 0) {
        // Real identity: never provisioned, so existing bindings stay untouched.
        profile.device_id = SystemInfo::GetMacAddress();
        Settings board("board", false);
        profile.uuid = board.GetString("uuid");
        return profile;
    }

    Settings settings("identity", true);
    auto mac_key = SlotKey(slot, "mac");
    auto uuid_key = SlotKey(slot, "uuid");
    auto mode_key = SlotKey(slot, "mode");

    profile.device_id = settings.GetString(mac_key);
    profile.uuid = settings.GetString(uuid_key);
    int mode = settings.GetInt(mode_key, 0);

    bool provisioned = false;
    if (profile.device_id.empty()) {
        profile.device_id = GenerateDeviceId();
        settings.SetString(mac_key, profile.device_id);
        provisioned = true;
    }
    if (profile.uuid.empty()) {
        profile.uuid = GenerateUuid();
        settings.SetString(uuid_key, profile.uuid);
        provisioned = true;
    }
    if (mode != 1 && mode != 2) {
        mode = 1;  // first use of a synthetic slot: force activation v1
        settings.SetInt(mode_key, mode);
        provisioned = true;
    }
    if (provisioned) {
        ESP_LOGI(TAG, "Provisioned slot %d: device_id=%s uuid=%s", slot,
                 profile.device_id.c_str(), profile.uuid.c_str());
    }

    profile.force_v1 = (mode == 1);
    return profile;
}

Identity::Profile Identity::Active() {
    return Load(GetSlot());
}

bool Identity::ForcesV1() {
    return Active().force_v1;
}

std::string Identity::CredentialNamespace(const std::string& base) {
    int slot = GetSlot();
    std::string target = base + std::to_string(slot);
    if (slot == 0) {
        MigrateLegacyCredentials(base, target);
    }
    return target;
}

bool Identity::SelectSlot(int slot) {
    if (slot < 0 || slot >= GetCount()) {
        ESP_LOGW(TAG, "Rejecting slot %d (count=%d)", slot, GetCount());
        return false;
    }
    Settings settings("identity", true);
    settings.SetInt("slot", slot);
    ESP_LOGI(TAG, "Identity slot selected: %d", slot);
    return true;
}

std::string Identity::GetName(int slot) {
    if (slot < 0 || slot >= kMaxCount) {
        return "";
    }
    Settings settings("identity", false);
    auto name = settings.GetString(NameKey(slot));
    if (!name.empty()) {
        return name;
    }
    return kDefaultNames[slot];
}

bool Identity::SetName(int slot, const std::string& name) {
    if (slot < 0 || slot >= GetCount()) {
        ESP_LOGW(TAG, "Rejecting rename for slot %d (count=%d)", slot, GetCount());
        return false;
    }
    if (name.empty() || name.size() > kMaxNameBytes) {
        ESP_LOGW(TAG, "Rejecting identity name of %zu bytes", name.size());
        return false;
    }
    Settings settings("identity", true);
    settings.SetString(NameKey(slot), name);
    ESP_LOGI(TAG, "Identity slot %d renamed to %s", slot, name.c_str());
    return true;
}

bool Identity::IsConfigured(int slot) {
    if (slot < 0 || slot >= GetCount()) {
        return false;
    }
    // Read-only on purpose: the selection UI must never write NVS.
    Settings settings("mqtt" + std::to_string(slot), false);
    return !settings.GetString("endpoint").empty();
}

void Identity::LogActive(bool has_serial_number) {
    const Profile profile = Active();
    const int activation_version = (profile.force_v1 || !has_serial_number) ? 1 : 2;
    ESP_LOGI(TAG, "slot=%d name=%s device_id=%s uuid=%s mode=%s activation_version=%d",
             profile.slot, GetName(profile.slot).c_str(), profile.device_id.c_str(),
             profile.uuid.c_str(), profile.force_v1 ? "v1" : "hw", activation_version);
}
