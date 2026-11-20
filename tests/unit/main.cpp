#include <iostream>

#include "Check.hpp"

namespace
{

int g_failures = 0;
const char* g_currentTest = "";

}

// A function-local static is built on first use, so registrars in other
// files never see it uninitialized.
std::vector<std::pair<const char*, TestFunction> >& testRegistry()
{
	static std::vector<std::pair<const char*, TestFunction> > registry;
	return registry;
}

TestRegistrar::TestRegistrar(const char* name, TestFunction function)
{
	testRegistry().push_back(std::make_pair(name, function));
}

void reportFailure(const char* file, int line, const std::string& message)
{
	++g_failures;
	std::cerr << file << ":" << line << ": [" << g_currentTest << "] " << message << '\n';
}

int main()
{
	const std::vector<std::pair<const char*, TestFunction> >& tests = testRegistry();

	for (std::vector<std::pair<const char*, TestFunction> >::size_type i = 0; i < tests.size(); ++i)
	{
		g_currentTest = tests[i].first;
		tests[i].second();
	}
	std::cout << tests.size() << " tests, " << g_failures << " failed checks" << std::endl;
	return g_failures == 0 ? 0 : 1;
}
