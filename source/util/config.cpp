#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "util/config.hpp"
#include "util/json.hpp"

namespace inst::config {
    std::string gAuthKey;
    std::string lastNetUrl;
    std::string offlineDbManifestUrl;
    std::string remoteUrl;
    std::string remoteProfileFile;
    std::string remoteUser;
    std::string remotePass;
    std::string httpUserAgentMode;
    std::string httpUserAgent;
    std::vector<std::string> updateInfo;
    int languageSetting;
    bool autoUpdate;
    bool deletePrompt;
    bool gayMode;
    bool soundEnabled;
    bool oledMode;
    bool mtpExposeAlbum;
    bool usbHddShowHiddenFiles = false;
    std::string themeFile = "theme.json";
    bool ignoreReqVers;
    bool overClock;
    bool usbAck;
    bool validateNCAs;
    bool remoteHideInstalled;
    bool remoteHideInstalledSection;
    bool remoteHideIncompatibleCheats;
    bool remoteAllBaseOnly;
    bool remoteLegacyMode;
    bool remoteStartGridMode;
    int remoteAllSortMode = 3;
    int remoteBrowseSortMode = 0;
    bool offlineDbAutoCheckOnStartup;
    bool verboseInstallLogging;

    namespace {
        std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string Trim(const std::string& value)
        {
            if (value.empty())
                return "";
            std::size_t start = value.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
                return "";
            std::size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(start, (end - start) + 1);
        }

        std::string NormalizeProtocol(const std::string& protocol)
        {
            std::string lower = ToLower(Trim(protocol));
            if (lower == "https")
                return "https";
            return "http";
        }

        int DefaultPortForNormalizedProtocol(const std::string& normalizedProtocol)
        {
            return normalizedProtocol == "https" ? 443 : 8465;
        }

        bool IsLocalRemoteHost(std::string host)
        {
            const auto localIpv4 = [](std::uint32_t ip) {
                return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 16) == 0xc0a8 ||
                    (ip & 0xfff00000) == 0xac100000 || (ip >> 16) == 0xa9fe;
            };
            host = ToLower(Trim(host));
            if (host == "localhost") return true;
            in_addr ipv4{};
            if (inet_pton(AF_INET, host.c_str(), &ipv4) == 1) {
                return localIpv4(ntohl(ipv4.s_addr));
            }
            if (!host.empty() && host.front() == '[') {
                unsigned char ip[16]{};
                if (inet_pton(AF_INET6, host.substr(1, host.size() - 2).c_str(), ip) == 1) {
                    bool loopback = ip[15] == 1;
                    for (int i = 0; i < 15; ++i) loopback = loopback && ip[i] == 0;
                    bool mapped = ip[10] == 0xff && ip[11] == 0xff;
                    for (int i = 0; i < 10; ++i) mapped = mapped && ip[i] == 0;
                    if (mapped) return localIpv4((std::uint32_t(ip[12]) << 24) | (std::uint32_t(ip[13]) << 16) |
                        (std::uint32_t(ip[14]) << 8) | ip[15]);
                    return loopback || (ip[0] & 0xfe) == 0xfc || (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80);
                }
                return false;
            }
            return host.find('.') == std::string::npos || host.ends_with(".local") ||
                host.ends_with(".lan") || host.ends_with(".home");
        }

        bool EnsureRemotesDirectory()
        {
            std::error_code ec;
            if (std::filesystem::exists(inst::config::remotesDir, ec))
                return std::filesystem::is_directory(inst::config::remotesDir, ec);
            return std::filesystem::create_directories(inst::config::remotesDir, ec);
        }

        std::string RemoteDedupKey(const inst::config::RemoteProfile& remote)
        {
            return remote.lanUrl + "\x1f" + remote.wanUrl + "\x1f" + remote.connection + "\x1f" +
                inst::config::BuildRemoteUrl(remote) + "\x1f" + Trim(remote.username) + "\x1f" + Trim(remote.password) +
                "\x1f" + (remote.legacyMode ? "legacy" : "cyberfoil");
        }

        bool ParseBoolTextValue(const std::string& value, bool& out)
        {
            std::string lower = ToLower(Trim(value));
            if (lower == "true" || lower == "1" || lower == "yes") {
                out = true;
                return true;
            }
            if (lower == "false" || lower == "0" || lower == "no") {
                out = false;
                return true;
            }
            return false;
        }

        bool ParsePortValue(const std::string& value, int& out)
        {
            std::string trimmed = Trim(value);
            if (trimmed.empty())
                return false;
            char* end = nullptr;
            long parsed = std::strtol(trimmed.c_str(), &end, 10);
            if (end == trimmed.c_str() || (end != nullptr && *end != '\0'))
                return false;
            if (parsed <= 0 || parsed > 65535)
                return false;
            out = static_cast<int>(parsed);
            return true;
        }

        std::int64_t GetWriteTimestampSeconds(const std::filesystem::path& path)
        {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(path, ec);
            if (ec)
                return 0;
            auto now = std::chrono::system_clock::now();
            auto ftimeSys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - std::filesystem::file_time_type::clock::now() + now);
            return std::chrono::duration_cast<std::chrono::seconds>(ftimeSys.time_since_epoch()).count();
        }

        std::string SanitizeTitleStem(const std::string& title)
        {
            std::string stem;
            stem.reserve(title.size());
            for (unsigned char c : title) {
                if (std::isspace(c))
                    continue;
                if (std::isalnum(c) || c == '-' || c == '_')
                    stem.push_back(static_cast<char>(c));
                else
                    stem.push_back('_');
            }
            if (stem.empty())
                stem = "Remote";
            return stem;
        }

        std::string UniqueFileNameForTitle(const std::string& title)
        {
            const std::string base = SanitizeTitleStem(title);
            std::string candidate = base + ".json";
            int suffix = 2;
            while (std::filesystem::exists(inst::config::remotesDir + "/" + candidate)) {
                candidate = base + "_" + std::to_string(suffix) + ".json";
                suffix++;
            }
            return candidate;
        }

        nlohmann::json BuildRemoteJson(const inst::config::RemoteProfile& remote)
        {
            inst::config::RemoteProfile normalized = remote;
            normalized.protocol = NormalizeProtocol(normalized.protocol);
            normalized.host = Trim(normalized.host);
            normalized.path = inst::config::NormalizeRemotePath(normalized.path);
            normalized.title = Trim(normalized.title);
            if (normalized.port <= 0 || normalized.port > 65535)
                normalized.port = DefaultPortForNormalizedProtocol(normalized.protocol);

            return {
                {"remote", {
                    {"protocol", normalized.protocol},
                    {"host", normalized.host},
                    {"path", normalized.path},
                    {"port", normalized.port},
                    {"username", normalized.username},
                    {"password", normalized.password},
                    {"title", normalized.title},
                    {"favourite", normalized.favourite},
                    {"legacyMode", normalized.legacyMode},
                    {"lanUrl", normalized.lanUrl},
                    {"wanUrl", normalized.wanUrl},
                    {"connection", normalized.connection},
                    {"uid", normalized.uid}
                }}
            };
        }

        bool ParseRemoteUrlImpl(const std::string& rawUrl, std::string& protocol, std::string& host, int& port, std::string& path)
        {
            std::string url = Trim(rawUrl);
            if (url.empty())
                return false;

            path.clear();
            protocol = "http";
            if (url.rfind("https://", 0) == 0) {
                protocol = "https";
                url = url.substr(8);
            } else if (url.rfind("http://", 0) == 0) {
                protocol = "http";
                url = url.substr(7);
            }

            std::size_t slashPos = url.find('/');
            std::size_t queryPos = url.find_first_of("?#");
            std::size_t authorityEnd = std::string::npos;
            if (slashPos != std::string::npos)
                authorityEnd = slashPos;
            if (queryPos != std::string::npos && (authorityEnd == std::string::npos || queryPos < authorityEnd))
                authorityEnd = queryPos;

            if (slashPos != std::string::npos && slashPos == authorityEnd)
                path = inst::config::NormalizeRemotePath(url.substr(slashPos));
            if (authorityEnd != std::string::npos)
                url = url.substr(0, authorityEnd);

            std::size_t atPos = url.rfind('@');
            if (atPos != std::string::npos)
                url = url.substr(atPos + 1);

            if (url.empty())
                return false;

            host.clear();
            port = DefaultPortForNormalizedProtocol(protocol);
            if (!url.empty() && url.front() == '[') {
                std::size_t closingBracket = url.find(']');
                if (closingBracket == std::string::npos)
                    return false;
                host = url.substr(0, closingBracket + 1);
                if (closingBracket + 1 < url.size() && url[closingBracket + 1] == ':') {
                    int parsedPort = 0;
                    if (!ParsePortValue(url.substr(closingBracket + 2), parsedPort))
                        return false;
                    port = parsedPort;
                }
            } else {
                std::size_t colonPos = url.rfind(':');
                if (colonPos != std::string::npos && url.find(':') == colonPos) {
                    int parsedPort = 0;
                    if (!ParsePortValue(url.substr(colonPos + 1), parsedPort))
                        return false;
                    port = parsedPort;
                    host = url.substr(0, colonPos);
                } else {
                    host = url;
                }
            }

            host = Trim(host);
            if (host.empty())
                return false;
            return true;
        }

        bool ParseRemoteFile(const std::filesystem::path& path, inst::config::RemoteProfile& outRemote, bool* outNeedsRewrite = nullptr)
        {
            std::ifstream file(path);
            if (!file)
                return false;

            nlohmann::json root;
            try {
                file >> root;
            }
            catch (...) {
                return false;
            }

            if (!root.is_object())
                return false;

            bool needsRewrite = false;
            const nlohmann::json* remoteNode = &root;
            if (root.contains("remote")) {
                if (!root["remote"].is_object())
                    return false;
                remoteNode = &root["remote"];
            } else if (root.contains("shop")) {
                if (!root["shop"].is_object())
                    return false;
                remoteNode = &root["shop"];
                needsRewrite = true;
            } else if (root.contains("shops")) {
                if (!root["shops"].is_object())
                    return false;
                remoteNode = &root["shops"];
                needsRewrite = true;
            } else {
                needsRewrite = true;
            }

            try {
                inst::config::RemoteProfile parsed;
                parsed.protocol = "http";
                parsed.port = DefaultPortForNormalizedProtocol(parsed.protocol);
                parsed.favourite = false;

                const std::string rawProtocol = remoteNode->value("protocol", "http");
                parsed.protocol = NormalizeProtocol(rawProtocol);
                if (parsed.protocol != Trim(rawProtocol))
                    needsRewrite = true;
                parsed.port = DefaultPortForNormalizedProtocol(parsed.protocol);

                std::string parsedUrlProtocol;
                std::string parsedUrlHost;
                std::string parsedUrlPath;
                int parsedUrlPort = DefaultPortForNormalizedProtocol(parsed.protocol);

                if (remoteNode->contains("url") && (*remoteNode)["url"].is_string()) {
                    if (ParseRemoteUrlImpl((*remoteNode)["url"].get<std::string>(), parsedUrlProtocol, parsedUrlHost, parsedUrlPort, parsedUrlPath)) {
                        parsed.protocol = parsedUrlProtocol;
                        parsed.port = parsedUrlPort;
                        parsed.host = parsedUrlHost;
                        parsed.path = parsedUrlPath;
                        needsRewrite = true;
                    }
                }

                if (parsed.host.empty())
                    parsed.host = remoteNode->value("host", "");
                if (parsed.path.empty())
                    parsed.path = NormalizeRemotePath(remoteNode->value("path", ""));
                parsed.username = remoteNode->value("username", "");
                parsed.password = remoteNode->value("password", "");
                parsed.title = remoteNode->value("title", "");
                parsed.lanUrl = remoteNode->value("lanUrl", "");
                parsed.wanUrl = remoteNode->value("wanUrl", "");
                parsed.connection = remoteNode->value("connection", "auto");
                parsed.uid = remoteNode->value("uid", "");
                if (!remoteNode->contains("lanUrl") || !remoteNode->contains("wanUrl")) needsRewrite = true;
                if (remoteNode->contains("legacyMode") && (*remoteNode)["legacyMode"].is_boolean())
                    parsed.legacyMode = (*remoteNode)["legacyMode"].get<bool>();

                if (!remoteNode->contains("host") || !(*remoteNode)["host"].is_string())
                    needsRewrite = true;
                if (!remoteNode->contains("path") || !(*remoteNode)["path"].is_string())
                    needsRewrite = true;
                if (!remoteNode->contains("title") || !(*remoteNode)["title"].is_string())
                    needsRewrite = true;
                if (!remoteNode->contains("username") || !(*remoteNode)["username"].is_string())
                    needsRewrite = true;
                if (!remoteNode->contains("password") || !(*remoteNode)["password"].is_string())
                    needsRewrite = true;

                if (remoteNode->contains("port")) {
                    const auto& portValue = (*remoteNode)["port"];
                    int parsedPort = 0;
                    if (portValue.is_number_integer()) {
                        parsedPort = portValue.get<int>();
                        if (parsedPort > 0 && parsedPort <= 65535)
                            parsed.port = parsedPort;
                    } else if (portValue.is_number_unsigned()) {
                        std::uint32_t raw = portValue.get<std::uint32_t>();
                        if (raw > 0 && raw <= 65535)
                            parsed.port = static_cast<int>(raw);
                    } else if (portValue.is_string()) {
                        if (ParsePortValue(portValue.get<std::string>(), parsedPort))
                            parsed.port = parsedPort;
                        else
                            needsRewrite = true;
                    } else {
                        needsRewrite = true;
                    }
                } else {
                    needsRewrite = true;
                }

                const char* favouriteKeys[] = {"favourite", "favorite"};
                bool sawFavourite = false;
                for (const char* key : favouriteKeys) {
                    if (!remoteNode->contains(key))
                        continue;
                    sawFavourite = true;
                    const auto& favouriteValue = (*remoteNode)[key];
                    bool parsedFavourite = false;
                    if (favouriteValue.is_boolean()) {
                        parsed.favourite = favouriteValue.get<bool>();
                    } else if (favouriteValue.is_number_integer()) {
                        parsed.favourite = favouriteValue.get<int>() != 0;
                    } else if (favouriteValue.is_number_unsigned()) {
                        parsed.favourite = favouriteValue.get<unsigned int>() != 0;
                    } else if (favouriteValue.is_string() &&
                               ParseBoolTextValue(favouriteValue.get<std::string>(), parsedFavourite)) {
                        parsed.favourite = parsedFavourite;
                        needsRewrite = true;
                    } else {
                        needsRewrite = true;
                    }
                    if (std::string(key) == "favorite")
                        needsRewrite = true;
                    break;
                }
                if (!sawFavourite)
                    needsRewrite = true;

                std::string normalizedHost = Trim(parsed.host);
                if (normalizedHost != parsed.host)
                    needsRewrite = true;
                parsed.host = normalizedHost;

                std::string normalizedPath = NormalizeRemotePath(parsed.path);
                if (normalizedPath != parsed.path)
                    needsRewrite = true;
                parsed.path = normalizedPath;

                std::string normalizedTitle = Trim(parsed.title);
                if (normalizedTitle.empty()) {
                    normalizedTitle = parsed.path.empty() ? parsed.host : (parsed.host + parsed.path);
                    needsRewrite = true;
                } else if (normalizedTitle != parsed.title) {
                    needsRewrite = true;
                }
                parsed.title = normalizedTitle;

                if (parsed.port <= 0 || parsed.port > 65535)
                    parsed.port = DefaultPortForNormalizedProtocol(parsed.protocol);
                if (!NormalizeRemoteProfile(parsed) || parsed.title.empty())
                    return false;

                parsed.fileName = path.filename().string();
                parsed.updatedAt = GetWriteTimestampSeconds(path);
                outRemote = parsed;
                if (outNeedsRewrite != nullptr)
                    *outNeedsRewrite = needsRewrite;
                return true;
            } catch (...) {
                return false;
            }
        }

        void MigrateLegacyRemoteFiles()
        {
            const std::filesystem::path sourceDirs[] = {
                std::filesystem::path(inst::config::remotesDir),
                std::filesystem::path(inst::config::legacyShopsDir)
            };

            for (const auto& sourceDir : sourceDirs) {
                std::error_code ec;
                if (!std::filesystem::exists(sourceDir, ec) || !std::filesystem::is_directory(sourceDir, ec))
                    continue;

                for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
                    if (ec)
                        break;
                    if (!entry.is_regular_file())
                        continue;

                    bool needsRewrite = false;
                    inst::config::RemoteProfile parsed;
                    if (!ParseRemoteFile(entry.path(), parsed, &needsRewrite))
                        continue;
                    if (!needsRewrite && sourceDir != std::filesystem::path(inst::config::legacyShopsDir))
                        continue;

                    parsed.fileName = entry.path().filename().string();
                    std::string ignored;
                    if (!inst::config::SaveRemote(parsed, &ignored))
                        continue;

                    if (sourceDir == std::filesystem::path(inst::config::legacyShopsDir)) {
                        std::error_code removeEc;
                        std::filesystem::remove(entry.path(), removeEc);
                    }
                }
            }

            std::error_code emptyCheckEc;
            if (std::filesystem::exists(std::filesystem::path(inst::config::legacyShopsDir), emptyCheckEc) &&
                std::filesystem::is_empty(std::filesystem::path(inst::config::legacyShopsDir), emptyCheckEc)) {
                std::error_code removeDirEc;
                std::filesystem::remove(std::filesystem::path(inst::config::legacyShopsDir), removeDirEc);
            }
        }

        void MigrateLegacyRemoteIcons()
        {
            const std::filesystem::path legacyDir(inst::config::legacyShopIconsDir);
            if (!std::filesystem::exists(legacyDir))
                return;

            std::error_code ec;
            std::filesystem::create_directories(inst::config::remoteIconsDir, ec);
            if (ec)
                return;

            const std::filesystem::path newDir(inst::config::remoteIconsDir);
            for (const auto& entry : std::filesystem::directory_iterator(legacyDir, ec)) {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;

                const std::filesystem::path destination = newDir / entry.path().filename();
                std::error_code copyEc;
                if (!std::filesystem::exists(destination)) {
                    std::filesystem::copy_file(entry.path(), destination, std::filesystem::copy_options::none, copyEc);
                    if (copyEc)
                        continue;
                }

                std::error_code removeEc;
                std::filesystem::remove(entry.path(), removeEc);
            }

            std::error_code emptyCheckEc;
            if (std::filesystem::exists(legacyDir, emptyCheckEc) && std::filesystem::is_empty(legacyDir, emptyCheckEc)) {
                std::error_code removeDirEc;
                std::filesystem::remove(legacyDir, removeDirEc);
            }
        }

        void SortRemoteProfiles(std::vector<inst::config::RemoteProfile>& remotes)
        {
            std::sort(remotes.begin(), remotes.end(), [](const auto& a, const auto& b) {
                if (a.favourite != b.favourite)
                    return a.favourite > b.favourite;

                if (a.favourite && b.favourite) {
                    std::string aTitle = ToLower(a.title);
                    std::string bTitle = ToLower(b.title);
                    if (aTitle != bTitle)
                        return aTitle < bTitle;
                    return a.fileName < b.fileName;
                }

                if (a.updatedAt != b.updatedAt)
                    return a.updatedAt > b.updatedAt;
                std::string aTitle = ToLower(a.title);
                std::string bTitle = ToLower(b.title);
                if (aTitle != bTitle)
                    return aTitle < bTitle;
                return a.fileName < b.fileName;
            });
        }

        void TryMigrateLegacyRemoteToJson()
        {
            if (inst::config::remoteUrl.empty())
                return;

            std::vector<inst::config::RemoteProfile> remotes = inst::config::LoadRemotes();
            for (const auto& remote : remotes) {
                if (!remoteProfileFile.empty() && remote.fileName == remoteProfileFile) return;
                if (inst::config::RemoteMatchesUrl(remote, inst::config::remoteUrl) &&
                    remote.username == inst::config::remoteUser &&
                    remote.password == inst::config::remotePass) {
                    return;
                }
            }

            std::string protocol;
            std::string host;
            std::string path;
            int port = DefaultPortForProtocol("http");
            if (!ParseRemoteUrl(inst::config::remoteUrl, protocol, host, port, path))
                return;

            inst::config::RemoteProfile migrated;
            migrated.protocol = protocol;
            migrated.host = host;
            migrated.path = path;
            migrated.port = port;
            migrated.username = inst::config::remoteUser;
            migrated.password = inst::config::remotePass;
            migrated.legacyMode = inst::config::remoteLegacyMode;
            migrated.title = path.empty() ? host : (host + path);
            migrated.favourite = false;

            std::string ignored;
            inst::config::SaveRemote(migrated, &ignored);
        }
    }

    int DefaultPortForProtocol(const std::string& protocol)
    {
        return DefaultPortForNormalizedProtocol(NormalizeProtocol(protocol));
    }

    std::string NormalizeHttpUserAgentMode(const std::string& mode)
    {
        const std::string normalized = ToLower(Trim(mode));
        if (normalized == "chrome")
            return "chrome";
        if (normalized == "safari")
            return "safari";
        if (normalized == "tinfoil")
            return "tinfoil";
        if (normalized == "firefox")
            return "firefox";
        if (normalized == "custom")
            return "custom";
        return "default";
    }

    std::string NormalizeRemotePath(const std::string& value)
    {
        std::string normalized = Trim(value);
        if (normalized.empty())
            return "";

        std::size_t suffixPos = normalized.find_first_of("?#");
        if (suffixPos != std::string::npos)
            normalized = normalized.substr(0, suffixPos);

        normalized = Trim(normalized);
        if (normalized.empty() || normalized == "/")
            return "";
        if (normalized.front() != '/')
            normalized.insert(normalized.begin(), '/');
        while (normalized.size() > 1 && normalized.back() == '/')
            normalized.pop_back();
        return normalized == "/" ? "" : normalized;
    }

    bool ParseRemoteUrl(const std::string& rawUrl, std::string& protocol, std::string& host, int& port, std::string& path)
    {
        return ParseRemoteUrlImpl(rawUrl, protocol, host, port, path);
    }

    std::string BuildRemoteUrl(const RemoteProfile& remote)
    {
        if (!remote.lanUrl.empty() || !remote.wanUrl.empty()) {
            if (remote.connection == "lan") return remote.lanUrl;
            if (remote.connection == "wan") return remote.wanUrl;
            return remote.lanUrl.empty() ? remote.wanUrl : remote.lanUrl;
        }
        std::string host = Trim(remote.host);
        if (host.empty())
            return "";

        std::string protocol = NormalizeProtocol(remote.protocol);
        std::string path = NormalizeRemotePath(remote.path);
        int port = remote.port;
        if (port <= 0 || port > 65535)
            port = DefaultPortForNormalizedProtocol(protocol);

        return protocol + "://" + host + ":" + std::to_string(port) + path;
    }

    bool NormalizeRemoteEndpoint(const std::string& input, std::string& output)
    {
        const std::string url = Trim(input);
        output.clear();
        if (url.empty()) return true;
        if (url.size() > 512 || (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)) return false;
        for (unsigned char c : url) if (c <= 32 || c == 127 || c == '\\') return false;
        // Credentials are shared profile fields. Queries/fragments are not part
        // of the existing Remote base URL model, so reject rather than drop them.
        if (url.find_first_of("?#") != std::string::npos) return false;
        std::string protocol, host, path;
        int port = 0;
        if (!ParseRemoteUrl(url, protocol, host, port, path)) return false;
        const auto authorityStart = url.find("://") + 3;
        const auto authorityEnd = url.find('/', authorityStart);
        const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
        if (authority.find('@') != std::string::npos) return false;
        const bool explicitPort = host.front() == '[' ? authority.find(']') + 1 < authority.size()
            : authority.find(':') != std::string::npos;
        if (!explicitPort) port = protocol == "https" ? 443 : 80;
        if (host.front() == '[') {
            const auto end = authority.find(']');
            if (end == std::string::npos || (end + 1 < authority.size() && authority[end + 1] != ':')) return false;
            unsigned char address[16];
            if (inet_pton(AF_INET6, host.substr(1, host.size() - 2).c_str(), address) != 1) return false;
        } else {
            for (unsigned char c : host)
                if (!std::isalnum(c) && c != '.' && c != '-' && c != '_' && c < 128) return false;
        }
        if (port < 1 || port > 65535 || authority.back() == ':') return false;
        output = protocol + "://" + host + ":" + std::to_string(port) + NormalizeRemotePath(path);
        return true;
    }

    bool NormalizeRemoteAddress(const std::string& input, const std::string& protocol, std::string& output)
    {
        output.clear();
        if (protocol != "http" && protocol != "https") return false;
        const std::string address = Trim(input);
        if (address.empty()) return true;
        const std::string prefix = protocol + "://";
        if (address.find("://") != std::string::npos && address.rfind(prefix, 0) != 0) return false;
        return NormalizeRemoteEndpoint(address.rfind(prefix, 0) == 0 ? address : prefix + address, output);
    }

    bool NormalizeRemoteProfile(RemoteProfile& remote, std::string* error)
    {
        auto fail = [&](const char* message) { if (error) *error = message; return false; };
        if (remote.lanUrl.empty() && remote.wanUrl.empty() && !remote.host.empty()) {
            const std::string originalUrl = BuildRemoteUrl(remote);
            // Preserve the address while placing public HTTPS legacy servers in
            // Remote URL. HTTPS also remains valid for private/local LAN hosts.
            if (NormalizeProtocol(remote.protocol) == "https" && !IsLocalRemoteHost(remote.host))
                remote.wanUrl = originalUrl;
            else remote.lanUrl = originalUrl;
        }
        std::string lan, wan;
        if (!NormalizeRemoteEndpoint(remote.lanUrl, lan)) return fail("Invalid LAN address or port.");
        if (!NormalizeRemoteEndpoint(remote.wanUrl, wan)) return fail("Invalid Remote address or port.");
        if (!wan.empty() && wan.rfind("https://", 0) != 0) return fail("Remote URL must use HTTPS.");
        if (lan.empty() && wan.empty()) return fail("Enter a LAN URL or Remote URL.");
        if (remote.connection != "auto" && remote.connection != "lan" && remote.connection != "wan") return fail("Invalid connection preference.");
        if (remote.connection == "lan" && lan.empty()) return fail("LAN mode requires a LAN URL.");
        if (remote.connection == "wan" && wan.empty()) return fail("WAN mode requires a Remote URL.");
        remote.lanUrl = lan;
        remote.wanUrl = wan;
        const std::string preferred = BuildRemoteUrl(remote);
        // Keep the original host/port fields populated for older readers.
        ParseRemoteUrl(preferred, remote.protocol, remote.host, remote.port, remote.path);
        return true;
    }

    bool RemoteMatchesUrl(const RemoteProfile& remote, const std::string& url)
    {
        std::string normalized;
        if (!NormalizeRemoteEndpoint(url, normalized) || normalized.empty()) return false;
        return normalized == BuildRemoteUrl(remote) || normalized == remote.lanUrl || normalized == remote.wanUrl;
    }

    std::vector<RemoteProfile> LoadRemotes()
    {
        std::vector<RemoteProfile> remotes;
        if (!EnsureRemotesDirectory())
            return remotes;

        std::vector<std::string> seenKeys;
        const std::filesystem::path sourceDirs[] = {
            std::filesystem::path(inst::config::remotesDir),
            std::filesystem::path(inst::config::legacyShopsDir)
        };

        for (const auto& sourceDir : sourceDirs) {
            std::error_code ec;
            if (!std::filesystem::exists(sourceDir, ec) || !std::filesystem::is_directory(sourceDir, ec))
                continue;

            for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
                if (ec)
                    break;
                if (!entry.is_regular_file())
                    continue;

                RemoteProfile parsed;
                if (!ParseRemoteFile(entry.path(), parsed))
                    continue;

                const std::string dedupKey = RemoteDedupKey(parsed);
                if (std::find(seenKeys.begin(), seenKeys.end(), dedupKey) != seenKeys.end())
                    continue;
                seenKeys.push_back(dedupKey);
                remotes.push_back(std::move(parsed));
            }
        }

        SortRemoteProfiles(remotes);
        return remotes;
    }

    bool SaveRemote(const RemoteProfile& remote, std::string* error)
    {
        auto fail = [error](const std::string& message) {
            if (error != nullptr)
                *error = message;
            return false;
        };

        if (!EnsureRemotesDirectory())
            return fail("Failed to create remotes directory.");

        RemoteProfile normalized = remote;
        normalized.protocol = NormalizeProtocol(normalized.protocol);
        normalized.host = Trim(normalized.host);
        normalized.path = NormalizeRemotePath(normalized.path);
        normalized.title = Trim(normalized.title);
        if (normalized.port <= 0 || normalized.port > 65535)
            normalized.port = DefaultPortForNormalizedProtocol(normalized.protocol);

        std::string endpointError;
        if (!NormalizeRemoteProfile(normalized, &endpointError)) return fail(endpointError);
        if (normalized.title.empty())
            return fail("Title is required.");

        std::string fileName = std::filesystem::path(normalized.fileName).filename().string();
        if (fileName.empty()) {
            fileName = UniqueFileNameForTitle(normalized.title);
        } else if (std::filesystem::path(fileName).extension().empty()) {
            fileName += ".json";
        }

        std::filesystem::path remotePath = std::filesystem::path(inst::config::remotesDir) / fileName;
        std::ofstream file(remotePath, std::ios::out | std::ios::trunc);
        if (!file)
            return fail("Failed to open remote file for writing.");

        nlohmann::json j = BuildRemoteJson(normalized);

        file << std::setw(4) << j << std::endl;

        if (!file.good())
            return fail("Failed to write remote file.");

        return true;
    }

    bool DeleteRemote(const std::string& fileName)
    {
        std::string sanitized = std::filesystem::path(fileName).filename().string();
        if (sanitized.empty())
            return false;
        std::error_code ec;
        bool removed = false;
        const std::filesystem::path dirs[] = {
            std::filesystem::path(inst::config::remotesDir),
            std::filesystem::path(inst::config::legacyShopsDir)
        };

        for (const auto& dir : dirs) {
            std::filesystem::path remotePath = dir / sanitized;
            removed = std::filesystem::remove(remotePath, ec) || removed;
            ec.clear();
        }
        return removed;
    }

    bool SetActiveRemote(const RemoteProfile& remote, bool writeConfig)
    {
        RemoteProfile selected = remote;
        if (!NormalizeRemoteProfile(selected)) return false;
        std::string url = BuildRemoteUrl(selected);
        if (url.empty())
            return false;
        inst::config::remoteUrl = url;
        inst::config::remoteProfileFile = selected.fileName;
        if (remoteProfileFile.empty()) {
            for (const auto& saved : LoadRemotes()) {
                if (BuildRemoteUrl(saved) == url && saved.username == selected.username && saved.password == selected.password &&
                    saved.connection == selected.connection && saved.lanUrl == selected.lanUrl && saved.wanUrl == selected.wanUrl &&
                    saved.legacyMode == selected.legacyMode) {
                    remoteProfileFile = saved.fileName;
                    break;
                }
            }
        }
        inst::config::remoteUser = selected.username;
        inst::config::remotePass = selected.password;
        inst::config::remoteLegacyMode = selected.legacyMode;
        if (inst::config::remoteLegacyMode) {
            inst::config::httpUserAgentMode = "tinfoil";
            inst::config::httpUserAgent.clear();
        } else if (NormalizeHttpUserAgentMode(inst::config::httpUserAgentMode) == "tinfoil") {
            inst::config::httpUserAgentMode = "default";
            inst::config::httpUserAgent.clear();
        }
        if (writeConfig)
            inst::config::setConfig();
        return true;
    }

    void setConfig() {
        std::string persistedRemoteUrl = remoteUrl;
        if (!remoteProfileFile.empty()) {
            for (const auto& saved : LoadRemotes()) {
                if (saved.fileName == remoteProfileFile) {
                    persistedRemoteUrl = BuildRemoteUrl(saved);
                    break;
                }
            }
        }
        nlohmann::json j = {
            {"autoUpdate", autoUpdate},
            {"deletePrompt", deletePrompt},
            {"gAuthKey", gAuthKey},
            {"gayMode", gayMode},
            {"soundEnabled", soundEnabled},
            {"oledMode", oledMode},
            {"mtpExposeAlbum", mtpExposeAlbum},
            {"usbHddShowHiddenFiles", usbHddShowHiddenFiles},
            {"themeFile", themeFile},
            {"ignoreReqVers", ignoreReqVers},
            {"languageSetting", languageSetting},
            {"overClock", overClock},
            {"usbAck", usbAck},
            {"validateNCAs", validateNCAs},
            {"lastNetUrl", lastNetUrl},
            {"offlineDbManifestUrl", offlineDbManifestUrl},
            {"remoteUrl", persistedRemoteUrl},
            {"remoteProfileFile", remoteProfileFile},
            {"remoteUser", remoteUser},
            {"remotePass", remotePass},
            {"httpUserAgentMode", httpUserAgentMode},
            {"httpUserAgent", httpUserAgent},
            {"remoteHideInstalled", remoteHideInstalled},
            {"remoteHideInstalledSection", remoteHideInstalledSection},
            {"remoteHideIncompatibleCheats", remoteHideIncompatibleCheats},
            {"remoteAllBaseOnly", remoteAllBaseOnly},
            {"remoteLegacyMode", remoteLegacyMode},
            {"remoteStartGridMode", remoteStartGridMode},
            {"remoteAllSortMode", remoteAllSortMode},
            {"remoteBrowseSortMode", remoteBrowseSortMode},
            {"offlineDbAutoCheckOnStartup", offlineDbAutoCheckOnStartup},
            {"verboseInstallLogging", verboseInstallLogging},
            {"remoteRememberSelection", false},
            {"remoteSelection", nlohmann::json::array()}
        };
        std::ofstream file(inst::config::configPath);
        file << std::setw(4) << j << std::endl;
    }

    void parseConfig() {
        gAuthKey = {0x41,0x49,0x7a,0x61,0x53,0x79,0x42,0x4d,0x71,0x76,0x34,0x64,0x58,0x6e,0x54,0x4a,0x4f,0x47,0x51,0x74,0x5a,0x5a,0x53,0x33,0x43,0x42,0x6a,0x76,0x66,0x37,0x34,0x38,0x51,0x76,0x78,0x53,0x7a,0x46,0x30};
        languageSetting = 99;
        autoUpdate = true;
        deletePrompt = true;
        gayMode = true;
        soundEnabled = true;
        oledMode = true;
        mtpExposeAlbum = false;
        usbHddShowHiddenFiles = false;
        themeFile = "theme.json";
        ignoreReqVers = true;
        overClock = true;
        usbAck = false;
        validateNCAs = true;
        lastNetUrl = "https://";
        offlineDbManifestUrl = "https://github.com/luketanti/CyberFoil-DB/releases/latest/download/offline_db_manifest.json";
        remoteUrl.clear();
        remoteProfileFile.clear();
        remoteUser.clear();
        remotePass.clear();
        httpUserAgentMode = "default";
        httpUserAgent.clear();
        remoteHideInstalled = true;
        remoteHideInstalledSection = true;
        remoteHideIncompatibleCheats = false;
        remoteAllBaseOnly = true;
        remoteLegacyMode = false;
        remoteStartGridMode = false;
        remoteAllSortMode = 3; // Date New-Old
        remoteBrowseSortMode = 0;
        offlineDbAutoCheckOnStartup = true;
        verboseInstallLogging = false;
        bool hasHttpUserAgentModeKey = false;
        bool needsConfigRewrite = false;
        bool needsLegacyRemoteImport = false;

        try {
            std::ifstream file(inst::config::configPath);
            nlohmann::json j;
            file >> j;
            needsLegacyRemoteImport = j.is_object() && !j.contains("remoteProfileFile");
            if (j.contains("autoUpdate")) autoUpdate = j["autoUpdate"].get<bool>();
            if (j.contains("deletePrompt")) deletePrompt = j["deletePrompt"].get<bool>();
            if (j.contains("gAuthKey")) gAuthKey = j["gAuthKey"].get<std::string>();
            if (j.contains("gayMode")) gayMode = j["gayMode"].get<bool>();
            if (j.contains("soundEnabled")) soundEnabled = j["soundEnabled"].get<bool>();
            if (j.contains("oledMode")) oledMode = j["oledMode"].get<bool>();
            if (j.contains("mtpExposeAlbum")) mtpExposeAlbum = j["mtpExposeAlbum"].get<bool>();
            if (j.contains("usbHddShowHiddenFiles")) usbHddShowHiddenFiles = j["usbHddShowHiddenFiles"].get<bool>();
            if (j.contains("themeFile") && j["themeFile"].is_string()) themeFile = j["themeFile"].get<std::string>();
            if (j.contains("ignoreReqVers")) ignoreReqVers = j["ignoreReqVers"].get<bool>();
            if (j.contains("languageSetting")) languageSetting = j["languageSetting"].get<int>();
            if (j.contains("overClock")) overClock = j["overClock"].get<bool>();
            if (j.contains("usbAck")) usbAck = j["usbAck"].get<bool>();
            if (j.contains("validateNCAs")) validateNCAs = j["validateNCAs"].get<bool>();
            if (j.contains("lastNetUrl")) lastNetUrl = j["lastNetUrl"].get<std::string>();
            if (j.contains("offlineDbManifestUrl")) offlineDbManifestUrl = j["offlineDbManifestUrl"].get<std::string>();
            if (j.contains("remoteUrl")) remoteUrl = j["remoteUrl"].get<std::string>();
            if (j.contains("remoteProfileFile")) remoteProfileFile = j["remoteProfileFile"].get<std::string>();
            if (j.contains("remoteUser")) remoteUser = j["remoteUser"].get<std::string>();
            if (j.contains("remotePass")) remotePass = j["remotePass"].get<std::string>();
            if (j.contains("httpUserAgentMode")) {
                httpUserAgentMode = j["httpUserAgentMode"].get<std::string>();
                hasHttpUserAgentModeKey = true;
            }
            if (j.contains("httpUserAgent")) httpUserAgent = j["httpUserAgent"].get<std::string>();
            if (j.contains("remoteHideInstalled")) remoteHideInstalled = j["remoteHideInstalled"].get<bool>();
            if (j.contains("remoteHideInstalledSection")) remoteHideInstalledSection = j["remoteHideInstalledSection"].get<bool>();
            if (j.contains("remoteHideIncompatibleCheats")) remoteHideIncompatibleCheats = j["remoteHideIncompatibleCheats"].get<bool>();
            if (j.contains("remoteAllBaseOnly")) remoteAllBaseOnly = j["remoteAllBaseOnly"].get<bool>();
            if (j.contains("remoteLegacyMode")) remoteLegacyMode = j["remoteLegacyMode"].get<bool>();
            if (j.contains("remoteStartGridMode")) remoteStartGridMode = j["remoteStartGridMode"].get<bool>();
            if (j.contains("remoteAllSortMode") && j["remoteAllSortMode"].is_number_integer()) remoteAllSortMode = j["remoteAllSortMode"].get<int>();
            if (j.contains("remoteBrowseSortMode") && j["remoteBrowseSortMode"].is_number_integer()) remoteBrowseSortMode = j["remoteBrowseSortMode"].get<int>();
            if (j.contains("offlineDbAutoCheckOnStartup")) offlineDbAutoCheckOnStartup = j["offlineDbAutoCheckOnStartup"].get<bool>();
            if (j.contains("verboseInstallLogging")) verboseInstallLogging = j["verboseInstallLogging"].get<bool>();

            static const char* currentKeys[] = {
                "autoUpdate",
                "deletePrompt",
                "gAuthKey",
                "gayMode",
                "soundEnabled",
                "oledMode",
                "mtpExposeAlbum",
                "usbHddShowHiddenFiles",
                "themeFile",
                "ignoreReqVers",
                "languageSetting",
                "overClock",
                "usbAck",
                "validateNCAs",
                "lastNetUrl",
                "offlineDbManifestUrl",
                "remoteUrl",
                "remoteProfileFile",
                "remoteUser",
                "remotePass",
                "httpUserAgentMode",
                "httpUserAgent",
                "remoteHideInstalled",
                "remoteHideInstalledSection",
                "remoteHideIncompatibleCheats",
                "remoteAllBaseOnly",
                "remoteLegacyMode",
                "remoteStartGridMode",
                "remoteAllSortMode",
                "remoteBrowseSortMode",
                "offlineDbAutoCheckOnStartup",
                "verboseInstallLogging"
            };

            for (const char* key : currentKeys) {
                if (!j.contains(key)) {
                    needsConfigRewrite = true;
                    break;
                }
            }
        }
        catch (...) {
            needsLegacyRemoteImport = false;
            // If loading values from the config fails, we just load the defaults and overwrite the old config
            setConfig();
        }

        httpUserAgentMode = NormalizeHttpUserAgentMode(httpUserAgentMode);
        if (!hasHttpUserAgentModeKey && !Trim(httpUserAgent).empty())
            httpUserAgentMode = "custom";

        if (!Trim(remoteUrl).empty()) {
            std::string protocol;
            std::string host;
            std::string path;
            int port = DefaultPortForProtocol("http");
            if (ParseRemoteUrl(remoteUrl, protocol, host, port, path)) {
                RemoteProfile normalizedRemote;
                normalizedRemote.protocol = protocol;
                normalizedRemote.host = host;
                normalizedRemote.path = path;
                normalizedRemote.port = port;
                const std::string normalizedRemoteUrl = BuildRemoteUrl(normalizedRemote);
                if (normalizedRemoteUrl != remoteUrl)
                    needsConfigRewrite = true;
                remoteUrl = normalizedRemoteUrl;
            }
        }

        EnsureRemotesDirectory();
        MigrateLegacyRemoteFiles();
        if (needsLegacyRemoteImport) TryMigrateLegacyRemoteToJson();
        MigrateLegacyRemoteIcons();
        if (remoteProfileFile.empty()) {
            for (const auto& saved : LoadRemotes()) {
                if (RemoteMatchesUrl(saved, remoteUrl) && saved.username == remoteUser && saved.password == remotePass &&
                    saved.legacyMode == remoteLegacyMode) {
                    remoteProfileFile = saved.fileName;
                    needsConfigRewrite = true;
                    break;
                }
            }
        }
        if (needsConfigRewrite)
            setConfig();
    }
}
