#ifndef _SD_NOTE_STORE_H_
#define _SD_NOTE_STORE_H_

#include <expected>
#include <string>

#include <sdmmc_cmd.h>

// Small SD-card note store used by the board-level inspiration/insight tools.
// The card is mounted once at boot; the note tools are only registered when
// the mount succeeds.
class SdNoteStore {
public:
    SdNoteStore() = default;
    ~SdNoteStore();

    SdNoteStore(const SdNoteStore&) = delete;
    SdNoteStore& operator=(const SdNoteStore&) = delete;

    bool Mount();
    bool mounted() const { return card_ != nullptr; }

    std::expected<std::string, std::string> SaveNote(const std::string& type,
                                                     const std::string& display_name,
                                                     const std::string& title,
                                                     const std::string& content,
                                                     const std::string& tags);

private:
    bool EnsureDirectory(const std::string& path);
    void EnsureReadme();

    sdmmc_card_t* card_ = nullptr;
};

#endif  // _SD_NOTE_STORE_H_
