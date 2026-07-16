#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string Sha256Hex(const uint8_t* data, size_t size);
std::string Sha256Hex(const std::string& data);
