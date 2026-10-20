// util.hpp —— 字符串与哈希工具（无第三方依赖，PC / Switch 通用）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace se {

std::string md5_hex(const std::string& in);
std::string sha1_hex(const std::string& in);
std::string base64_encode(const std::string& in);

// 与 JS encodeURIComponent 行为一致：保留 A-Za-z0-9-_.~，其余 %XX
std::string url_encode(const std::string& in);

// 只保留数字字符（QQ 的 uin 形如 o123456789）
std::string digits_only(const std::string& in);

// QQ 扫码用：种子 0
uint32_t hash33(const std::string& s);
// QQ g_tk 用：种子 5381（djb2）。两者不是一个算法，别混用。
uint32_t gtk_of(const std::string& skey);

std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string to_lower(std::string s);

}  // namespace se
