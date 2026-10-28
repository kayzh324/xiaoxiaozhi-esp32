#ifndef IDENTITY_H
#define IDENTITY_H

#include <string>

/**
 * Multi-identity ("profile") rotation.
 *
 * Slot 0 is the real identity: factory Wi-Fi MAC + the existing NVS board/uuid.
 * It is never provisioned, so an untouched device behaves exactly as before.
 * Slots 1..count-1 get a locally-administered MAC, a random UUID and force
 * activation v1 (no eFuse serial / HMAC), so they are separate devices to the
 * server and can each be bound with their own six-digit code.
 */
class Identity {
public:
    struct Profile {
        int slot;
        std::string device_id;
        std::string uuid;
        bool force_v1;  // id<N>_mode == 1
    };

    static int GetSlot();
    static int GetCount();
    static Profile Active();

    static bool ForcesV1();
    static std::string CredentialNamespace(const std::string& base);

    /// Display name for a slot (NVS identity/name<N>, default table below).
    static std::string GetName(int slot);
    static bool SetName(int slot, const std::string& name);
    /// True when the slot already holds server-issued MQTT credentials.
    /// Read-only: never triggers the legacy migration.
    static bool IsConfigured(int slot);

    static bool SelectSlot(int slot);
    static void LogActive(bool has_serial_number);

private:
    static Profile Load(int slot);
};

#endif  // IDENTITY_H
