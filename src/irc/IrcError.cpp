#include "irc/IrcError.hpp"

IrcError::IrcError(Numeric code, const std::string& params, const std::string& text)
	: code_(code), params_(params), text_(text)
{
}

IrcError::~IrcError() throw()
{
}

const char* IrcError::what() const throw()
{
	return text_.c_str();
}

Numeric IrcError::code() const
{
	return code_;
}

const std::string& IrcError::params() const
{
	return params_;
}

const std::string& IrcError::text() const
{
	return text_;
}
