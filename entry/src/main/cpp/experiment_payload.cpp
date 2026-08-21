#include "experiment_payload.h"

#include "wine_constants.h"

#include <dirent.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_NAPI"
#include <hilog/log.h>

namespace winehua {
namespace {

// HDC can write the physical sandbox path while the app can read this logical
// path. /data/local/tmp is denied to application processes by SELinux.
constexpr char kExperimentInbox[] = WINE_FILES_DIR "/experiment-inbox";
constexpr size_t kMaxArtifacts = 16;
constexpr off_t kMaxArtifactBytes = 256LL * 1024LL * 1024LL;
constexpr size_t kMaxHttpHeaderBytes = 32 * 1024;

bool IsSafeComponent(const std::string& value)
{
    if (value.empty() || value.size() > 80) return false;
    for (unsigned char ch : value)
    {
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= '0' && ch <= '9') && ch != '.' && ch != '_' && ch != '-')
            return false;
    }
    return value != "." && value != "..";
}

bool HasAllowedExtension(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string extension = name.substr(dot);
    return extension == ".exe" || extension == ".dll" || extension == ".json";
}

bool IsSha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (unsigned char ch : value)
    {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    }
    return true;
}

bool EnsureDirectoryTree(const std::string& path)
{
    if (path.empty() || path[0] != '/') return false;
    size_t position = 1;
    while (position <= path.size())
    {
        const size_t next = path.find('/', position);
        const std::string partial = path.substr(0, next == std::string::npos ? path.size() : next);
        if (!partial.empty() && mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
        if (next == std::string::npos) break;
        position = next + 1;
    }
    return true;
}

bool RemoveDirectoryTree(const std::string& path)
{
    DIR* directory = opendir(path.c_str());
    if (!directory) return errno == ENOENT;

    bool ok = true;
    while (dirent* entry = readdir(directory))
    {
        const char* name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        const std::string child = path + "/" + name;
        struct stat statBuffer = {};
        if (lstat(child.c_str(), &statBuffer) != 0)
        {
            ok = false;
            continue;
        }
        if (S_ISDIR(statBuffer.st_mode) && !S_ISLNK(statBuffer.st_mode))
            ok = RemoveDirectoryTree(child) && ok;
        else if (unlink(child.c_str()) != 0)
            ok = false;
    }
    closedir(directory);
    return (rmdir(path.c_str()) == 0 || errno == ENOENT) && ok;
}

class Sha256 {
public:
    Sha256()
    {
        state_ = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    }

    void Update(const uint8_t* data, size_t length)
    {
        bitLength_ += static_cast<uint64_t>(length) * 8;
        while (length)
        {
            const size_t copied = std::min(length, buffer_.size() - bufferLength_);
            memcpy(buffer_.data() + bufferLength_, data, copied);
            bufferLength_ += copied;
            data += copied;
            length -= copied;
            if (bufferLength_ == buffer_.size())
            {
                Transform(buffer_.data());
                bufferLength_ = 0;
            }
        }
    }

    std::string Final()
    {
        buffer_[bufferLength_++] = 0x80;
        if (bufferLength_ > 56)
        {
            while (bufferLength_ < buffer_.size()) buffer_[bufferLength_++] = 0;
            Transform(buffer_.data());
            bufferLength_ = 0;
        }
        while (bufferLength_ < 56) buffer_[bufferLength_++] = 0;
        for (int index = 7; index >= 0; --index)
            buffer_[bufferLength_++] = static_cast<uint8_t>(bitLength_ >> (index * 8));
        Transform(buffer_.data());

        static constexpr char hex[] = "0123456789abcdef";
        std::string value;
        value.reserve(64);
        for (uint32_t word : state_)
        {
            for (int index = 3; index >= 0; --index)
            {
                const uint8_t byte = static_cast<uint8_t>(word >> (index * 8));
                value.push_back(hex[byte >> 4]);
                value.push_back(hex[byte & 0x0f]);
            }
        }
        return value;
    }

private:
    static uint32_t RotateRight(uint32_t value, uint32_t amount)
    {
        return (value >> amount) | (value << (32 - amount));
    }

    void Transform(const uint8_t* block)
    {
        static constexpr uint32_t constants[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };
        uint32_t words[64] = {};
        for (size_t index = 0; index < 16; ++index)
            words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                (static_cast<uint32_t>(block[index * 4 + 2]) << 8) | block[index * 4 + 3];
        for (size_t index = 16; index < 64; ++index)
        {
            const uint32_t small0 = RotateRight(words[index - 15], 7) ^ RotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3);
            const uint32_t small1 = RotateRight(words[index - 2], 17) ^ RotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + small0 + words[index - 7] + small1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (size_t index = 0; index < 64; ++index)
        {
            const uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const uint32_t choice = (e & f) ^ ((~e) & g);
            const uint32_t temporary1 = h + sum1 + choice + constants[index] + words[index];
            const uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_;
    std::array<uint8_t, 64> buffer_ = {};
    size_t bufferLength_ = 0;
    uint64_t bitLength_ = 0;
};

struct LoopbackHttpEndpoint {
    uint16_t port = 0;
    std::string basePath;
};

bool ParseLoopbackHttpEndpoint(const std::string& sourceUrl, LoopbackHttpEndpoint* endpoint,
                               std::string* errorMessage)
{
    static constexpr char kPrefix[] = "http://127.0.0.1:";
    if (sourceUrl.rfind(kPrefix, 0) != 0 || sourceUrl.size() > 480)
    {
        *errorMessage = "experiment source must be a loopback HTTP URL";
        return false;
    }
    const size_t portStart = sizeof(kPrefix) - 1;
    const size_t pathStart = sourceUrl.find('/', portStart);
    const std::string portText = sourceUrl.substr(portStart, pathStart - portStart);
    if (portText.empty() || portText.size() > 5 ||
        !std::all_of(portText.begin(), portText.end(), [](unsigned char ch) { return std::isdigit(ch); }))
    {
        *errorMessage = "experiment source has an invalid port";
        return false;
    }
    const unsigned long port = strtoul(portText.c_str(), nullptr, 10);
    if (port == 0 || port > 65535)
    {
        *errorMessage = "experiment source port is out of range";
        return false;
    }
    std::string path = pathStart == std::string::npos ? "/" : sourceUrl.substr(pathStart);
    if (path.find("..") != std::string::npos || path.find('?') != std::string::npos ||
        path.find('#') != std::string::npos || path.find('%') != std::string::npos ||
        !std::all_of(path.begin(), path.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '-';
        }))
    {
        *errorMessage = "experiment source path contains unsupported characters";
        return false;
    }
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    endpoint->port = static_cast<uint16_t>(port);
    endpoint->basePath = path;
    return true;
}

bool DownloadAndVerify(const LoopbackHttpEndpoint& endpoint, const std::string& artifactName,
                       const std::string& destination, const std::string& expectedHash,
                       std::string* errorMessage)
{
    const std::string requestPath = endpoint.basePath + "/" + artifactName;
    const std::string temporary = destination + ".partial";
    int outputFd = -1;
    int socketFd = -1;
    auto fail = [&](const std::string& message) {
        if (socketFd >= 0) close(socketFd);
        if (outputFd >= 0) close(outputFd);
        unlink(temporary.c_str());
        *errorMessage = message;
        return false;
    };

    outputFd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (outputFd < 0) return fail("cannot create experiment download destination");
    socketFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socketFd < 0) return fail("cannot create experiment download socket");
    timeval timeout = {15, 0};
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
        connect(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
        return fail("cannot connect to experiment loopback source");

    const std::string request = "GET " + requestPath + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Connection: close\r\nAccept: application/octet-stream\r\n\r\n";
    size_t sent = 0;
    while (sent < request.size())
    {
        const ssize_t count = send(socketFd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) return fail("cannot request experiment artifact");
        sent += static_cast<size_t>(count);
    }

    std::array<char, 4096> input = {};
    std::string headers;
    size_t bodyOffset = std::string::npos;
    while ((bodyOffset = headers.find("\r\n\r\n")) == std::string::npos)
    {
        const ssize_t count = recv(socketFd, input.data(), input.size(), 0);
        if (count <= 0) return fail("experiment source returned an incomplete HTTP response");
        headers.append(input.data(), static_cast<size_t>(count));
        if (headers.size() > kMaxHttpHeaderBytes)
            return fail("experiment source returned oversized HTTP headers");
    }
    bodyOffset += 4;
    const size_t statusEnd = headers.find("\r\n");
    if (statusEnd == std::string::npos || headers.substr(0, statusEnd).find(" 200 ") == std::string::npos)
        return fail("experiment source did not return HTTP 200");

    uint64_t contentLength = 0;
    bool haveContentLength = false;
    size_t lineStart = statusEnd + 2;
    while (lineStart < bodyOffset - 2)
    {
        const size_t lineEnd = headers.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > bodyOffset - 2) break;
        std::string line = headers.substr(lineStart, lineEnd - lineStart);
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        static constexpr char kLength[] = "content-length:";
        if (line.rfind(kLength, 0) == 0)
        {
            size_t valueStart = sizeof(kLength) - 1;
            while (valueStart < line.size() && std::isspace(static_cast<unsigned char>(line[valueStart])))
                ++valueStart;
            const std::string value = line.substr(valueStart);
            if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
                return std::isdigit(ch);
            }))
                return fail("experiment source returned an invalid content length");
            contentLength = strtoull(value.c_str(), nullptr, 10);
            haveContentLength = true;
        }
        lineStart = lineEnd + 2;
    }
    if (!haveContentLength || contentLength == 0 || contentLength > static_cast<uint64_t>(kMaxArtifactBytes))
        return fail("experiment source returned an unsupported content length");

    Sha256 digest;
    uint64_t received = 0;
    auto writeBody = [&](const char* data, size_t length) {
        if (length > contentLength - received) return false;
        digest.Update(reinterpret_cast<const uint8_t*>(data), length);
        size_t written = 0;
        while (written < length)
        {
            const ssize_t count = write(outputFd, data + written, length - written);
            if (count <= 0) return false;
            written += static_cast<size_t>(count);
        }
        received += length;
        return true;
    };
    if (!writeBody(headers.data() + bodyOffset, headers.size() - bodyOffset))
        return fail("cannot write experiment download payload");
    while (received < contentLength)
    {
        const size_t expected = static_cast<size_t>(std::min<uint64_t>(input.size(), contentLength - received));
        const ssize_t count = recv(socketFd, input.data(), expected, 0);
        if (count <= 0 || !writeBody(input.data(), static_cast<size_t>(count)))
            return fail("experiment source returned an incomplete payload");
    }
    close(socketFd);
    socketFd = -1;
    if (fsync(outputFd) != 0) return fail("cannot flush experiment download payload");
    close(outputFd);
    outputFd = -1;
    if (digest.Final() != expectedHash)
        return fail("SHA-256 mismatch for downloaded experiment artifact");
    if (rename(temporary.c_str(), destination.c_str()) != 0)
        return fail("cannot finalize experiment download artifact");
    return true;
}

bool CopyAndVerify(const std::string& source, const std::string& destination,
                   const std::string& expectedHash, std::string* errorMessage)
{
    int sourceFd = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (sourceFd < 0)
    {
        *errorMessage = "cannot open staged artifact: " + source;
        return false;
    }
    struct stat sourceStat = {};
    if (fstat(sourceFd, &sourceStat) != 0 || !S_ISREG(sourceStat.st_mode) ||
        sourceStat.st_size <= 0 || sourceStat.st_size > kMaxArtifactBytes)
    {
        close(sourceFd);
        *errorMessage = "staged artifact is not an allowed regular file: " + source;
        return false;
    }

    const std::string temporary = destination + ".partial";
    int destinationFd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (destinationFd < 0)
    {
        close(sourceFd);
        *errorMessage = "cannot create experiment artifact destination";
        return false;
    }

    Sha256 digest;
    std::array<uint8_t, 64 * 1024> buffer = {};
    bool ok = true;
    while (ok)
    {
        const ssize_t readBytes = read(sourceFd, buffer.data(), buffer.size());
        if (readBytes == 0) break;
        if (readBytes < 0)
        {
            ok = false;
            *errorMessage = "cannot read staged artifact";
            break;
        }
        digest.Update(buffer.data(), static_cast<size_t>(readBytes));
        size_t written = 0;
        while (written < static_cast<size_t>(readBytes))
        {
            const ssize_t writeBytes = write(destinationFd, buffer.data() + written,
                static_cast<size_t>(readBytes) - written);
            if (writeBytes <= 0)
            {
                ok = false;
                *errorMessage = "cannot write experiment artifact";
                break;
            }
            written += static_cast<size_t>(writeBytes);
        }
    }
    if (ok && fsync(destinationFd) != 0)
    {
        ok = false;
        *errorMessage = "cannot flush experiment artifact";
    }
    close(sourceFd);
    close(destinationFd);

    if (!ok || digest.Final() != expectedHash)
    {
        unlink(temporary.c_str());
        if (ok) *errorMessage = "SHA-256 mismatch for staged artifact: " + source;
        return false;
    }
    if (rename(temporary.c_str(), destination.c_str()) != 0)
    {
        unlink(temporary.c_str());
        *errorMessage = "cannot finalize experiment artifact";
        return false;
    }
    return true;
}

} // namespace

bool StageExperimentPayload(const std::string& experimentId,
                            const std::vector<ExperimentArtifact>& artifacts,
                            const std::string& prefixMode,
                            const std::string& sourceUrl,
                            std::string* errorMessage)
{
    if (errorMessage) errorMessage->clear();
    auto fail = [errorMessage](const std::string& message) {
        if (errorMessage) *errorMessage = message;
        OH_LOG_ERROR(LOG_APP, "[Experiment] %{public}s", message.c_str());
        return false;
    };
    if (!IsSafeComponent(experimentId) || artifacts.empty() || artifacts.size() > kMaxArtifacts)
        return fail("invalid experiment identifier or artifact count");

    bool hasExecutable = false;
    for (size_t index = 0; index < artifacts.size(); ++index)
    {
        const ExperimentArtifact& artifact = artifacts[index];
        if (!IsSafeComponent(artifact.name) || !HasAllowedExtension(artifact.name) ||
            !IsSha256(artifact.sha256))
            return fail("invalid experiment artifact manifest");
        for (size_t previous = 0; previous < index; ++previous)
        {
            if (artifacts[previous].name == artifact.name)
                return fail("duplicate experiment artifact name");
        }
        if (artifact.name.size() > 4 && artifact.name.substr(artifact.name.size() - 4) == ".exe")
            hasExecutable = true;
    }
    if (!hasExecutable) return fail("experiment manifest does not include an executable");

    LoopbackHttpEndpoint endpoint;
    const bool downloadSource = !sourceUrl.empty();
    std::string endpointError;
    if (downloadSource && !ParseLoopbackHttpEndpoint(sourceUrl, &endpoint, &endpointError))
        return fail(endpointError);

    const std::string prefix = prefixMode == "clean" ? WINE_SMOKE_PREFIX : WINE_PREFIX;
    const std::string sourceDirectory = std::string(kExperimentInbox) + "/" + experimentId;
    const std::string experimentsRoot = prefix + "/drive_c/smoke/experiments";
    const std::string destination = experimentsRoot + "/" + experimentId;
    const std::string temporary = destination + ".tmp";

    if (!EnsureDirectoryTree(experimentsRoot)) return fail("cannot create experiment destination root");
    RemoveDirectoryTree(temporary);
    if (mkdir(temporary.c_str(), 0700) != 0)
        return fail("cannot create experiment staging directory");

    for (const ExperimentArtifact& artifact : artifacts)
    {
        std::string copyError;
        const bool staged = downloadSource
            ? DownloadAndVerify(endpoint, artifact.name, temporary + "/" + artifact.name,
                                artifact.sha256, &copyError)
            : CopyAndVerify(sourceDirectory + "/" + artifact.name,
                            temporary + "/" + artifact.name, artifact.sha256, &copyError);
        if (!staged)
        {
            RemoveDirectoryTree(temporary);
            return fail(copyError);
        }
        OH_LOG_INFO(LOG_APP, "[Experiment] staged id=%{public}s artifact=%{public}s sha256=%{public}s",
                    experimentId.c_str(), artifact.name.c_str(), artifact.sha256.c_str());
    }
    if (!EnsureDirectoryTree(temporary + "/results"))
    {
        RemoveDirectoryTree(temporary);
        return fail("cannot create experiment result directory");
    }
    if (!RemoveDirectoryTree(destination))
    {
        RemoveDirectoryTree(temporary);
        return fail("cannot replace prior experiment payload");
    }
    if (rename(temporary.c_str(), destination.c_str()) != 0)
    {
        RemoveDirectoryTree(temporary);
        return fail("cannot activate experiment payload");
    }
    OH_LOG_INFO(LOG_APP, "[Experiment] ready id=%{public}s target=%{public}s",
                experimentId.c_str(), destination.c_str());
    return true;
}

} // namespace winehua
