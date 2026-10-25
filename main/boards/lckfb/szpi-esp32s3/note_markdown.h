#ifndef _NOTE_MARKDOWN_H_
#define _NOTE_MARKDOWN_H_

#include <cctype>
#include <string>
#include <vector>

// Pure Markdown/path helpers for the SD-card note tools. Keeping this header
// free of ESP-IDF dependencies makes the formatting rules easy to check on the
// host as well as on the device.
namespace note_markdown {

inline std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

inline std::string FirstLine(const std::string& value) {
    size_t end = value.find_first_of("\r\n");
    return Trim(value.substr(0, end));
}

// Cut at a UTF-8 code-point boundary so a Chinese title never becomes invalid
// text just because it was longer than the display/filename limit.
inline std::string TruncateUtf8(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0) == 0x80) {
        --end;
    }
    return value.substr(0, end);
}

inline std::string NormalizeTitle(const std::string& title, const std::string& content,
                                  const char* fallback) {
    std::string normalized = FirstLine(title);
    if (normalized.empty()) {
        normalized = FirstLine(content);
    }
    if (normalized.empty()) {
        normalized = fallback;
    }
    return TruncateUtf8(normalized, 120);
}

inline std::string YamlQuote(const std::string& value) {
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char c : value) {
        if (c == '\\' || c == '"') {
            quoted.push_back('\\');
        }
        if (c == '\r' || c == '\n' || c == '\t') {
            quoted.push_back(' ');
            continue;
        }
        quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
}

inline std::vector<std::string> ParseTags(const std::string& raw) {
    std::vector<std::string> tags;
    size_t start = 0;
    while (start <= raw.size() && tags.size() < 8) {
        size_t end = raw.find(',', start);
        std::string tag =
            Trim(raw.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!tag.empty()) {
            tags.push_back(TruncateUtf8(tag, 40));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return tags;
}

inline std::string BuildMarkdown(const std::string& type, const std::string& display_name,
                                 const std::string& title, const std::string& content,
                                 const std::string& tags, const std::string& created_at) {
    std::string markdown;
    markdown.reserve(content.size() + 512);
    markdown += "---\n";
    markdown += "type: " + type + "\n";
    markdown += "title: " + YamlQuote(title) + "\n";
    markdown += "created_at: " + YamlQuote(created_at) + "\n";
    markdown += "tags: [";
    auto parsed_tags = ParseTags(tags);
    for (size_t i = 0; i < parsed_tags.size(); ++i) {
        if (i > 0) {
            markdown += ", ";
        }
        markdown += YamlQuote(parsed_tags[i]);
    }
    markdown += "]\n";
    markdown += "source: \"xiaozhi\"\n";
    markdown += "---\n\n# " + title + "\n\n";
    markdown += content;
    if (markdown.empty() || markdown.back() != '\n') {
        markdown += '\n';
    }
    markdown += "\n---\n\n*由小智「" + display_name + "」记录*\n";
    return markdown;
}

}  // namespace note_markdown

#endif  // _NOTE_MARKDOWN_H_
