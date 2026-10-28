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

    static bool SelectSlot(int slot);
    static int CycleSlot();
    static void LogActive(bool has_serial_number);

private:
    static Profile Load(int slot);
};

#endif  // IDENTITY_H
