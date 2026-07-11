#pragma once

#include "network/fetcher.h"

#include <cstddef>
#include <string>

bool IsOnionUrl(const std::string& url);
bool FetchViaEmbeddedArti(const std::string& url, size_t maxResponseBytes, FetchResult& out);
