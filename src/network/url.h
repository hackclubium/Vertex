#pragma once
#include <string>

bool HasUrlScheme(const std::string& url);
std::string ResolveUrlAgainstBase(const std::string& href, const std::string& base);
std::string UnwrapBingRedirect(const std::string& url);
std::string UnwrapDuckDuckGoRedirect(const std::string& url);
