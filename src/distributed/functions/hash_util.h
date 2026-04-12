#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace orion::distributed {

inline std::string compute_sha256(const std::string& data) {
#ifdef __APPLE__
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), data.size(), hash);

    std::stringstream ss;
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
#else
    return "hash-not-implemented-on-non-apple";
#endif
}

inline std::string compute_file_sha256(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    std::stringstream buffer;
    buffer << file.rdbuf();
    return compute_sha256(buffer.str());
}

} // namespace orion::distributed
