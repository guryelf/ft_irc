#ifndef IRCERROR_HPP
#define IRCERROR_HPP

#include <exception>
#include <string>

#include "irc/Numerics.hpp"

// A protocol error that becomes one numeric reply to the client who caused it.
// Command handlers throw it from their checks; Core::dispatch catches it and
// sends ":server <code> <nick> <params> :<text>". It never leaves the core.
class IrcError : public std::exception
{
public:
	IrcError(Numeric code, const std::string& params, const std::string& text);
	virtual ~IrcError() throw();

	virtual const char* what() const throw();
	Numeric code() const;
	const std::string& params() const;	// middle parameters, space-separated
	const std::string& text() const;	// trailing human-readable text

private:
	Numeric code_;
	std::string params_;
	std::string text_;
};

#endif
