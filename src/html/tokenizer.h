#pragma once
#include <string>
#include <map>
#include <functional>

enum class TokenType { Doctype, StartTag, EndTag, Text, Comment, EOF_ };

struct HtmlToken {
    TokenType   type = TokenType::Text;
    std::string name;           // tag name (start/end)
    std::string data;           // text content
    std::map<std::string, std::string> attrs;
    bool        selfClosing = false;
};

class HtmlTokenizer {
public:
    using Callback = std::function<void(const HtmlToken&)>;
    void tokenize(const std::string& html, Callback cb);

private:
    const std::string* m_src = nullptr;
    size_t             m_pos = 0;

    char        peek(int offset = 0) const;
    char        consume();
    bool        startsWith(const std::string& s) const;
    void        skipWS();
    std::string consumeName();
    std::string decodeEntities(const std::string& raw);
    std::map<std::string, std::string> consumeAttrs();
    void        skipUntil(const std::string& stop);
};
