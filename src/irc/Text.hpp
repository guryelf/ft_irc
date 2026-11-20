#ifndef TEXT_HPP
#define TEXT_HPP

#include <string>
#include <vector>

// IRC text rules shared by the core.

// rfc1459 case mapping: A-Z equal a-z, and [ ] \ ~ equal { } | ^.
// Nick and channel lookups always use this folded form as the key.
std::string ircLower(const std::string& text);

// RFC 2812 nickname: a letter or special first, then letters, digits,
// specials or '-'. Specials are [ ] \ ` _ ^ { | }.
bool isValidNick(const std::string& nick);

// '#' or '&' followed by at least one character, none of which is a space,
// comma, colon, NUL, BEL, CR or LF.
bool isValidChannelName(const std::string& name);

// True when a target names a channel ('#' or '&' first) rather than a nick.
bool looksLikeChannel(const std::string& target);

// "a,,b" gives {"a", "", "b"} and "" gives {}. Empty items are kept because
// JOIN matches keys to channels by position.
std::vector<std::string> splitComma(const std::string& list);

std::string toString(unsigned long value);

// ASCII-only upper case, independent of the locale.
std::string asciiUpper(const std::string& text);

// Makes client-supplied text safe as a middle parameter of a reply: text
// that is empty, contains a space, or starts with ':' would change how the
// reply parses, so it is replaced by "*".
std::string asParam(const std::string& text);

#endif
