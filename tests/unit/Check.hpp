#ifndef CHECK_HPP
#define CHECK_HPP

#include <sstream>
#include <string>
#include <utility>
#include <vector>

// A minimal C++98 test framework.
// TEST(name) { ... } registers a test; CHECK and CHECK_EQ record failures
// without stopping the test, so one run reports every broken expectation.

typedef void (*TestFunction)();

struct TestRegistrar
{
	TestRegistrar(const char* name, TestFunction function);
};

std::vector<std::pair<const char*, TestFunction> >& testRegistry();
void reportFailure(const char* file, int line, const std::string& message);

template <typename Actual, typename Expected>
void checkEqual(const Actual& actual, const Expected& expected, const char* expression,
	const char* file, int line)
{
	if (actual == expected)
		return;
	std::ostringstream message;
	message << expression << "\n    actual:   " << actual << "\n    expected: " << expected;
	reportFailure(file, line, message.str());
}

#define CHECK(condition) \
	do { if (!(condition)) reportFailure(__FILE__, __LINE__, #condition); } while (0)

#define CHECK_EQ(actual, expected) \
	checkEqual((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define TEST(name) \
	static void name(); \
	static TestRegistrar name##Registrar(#name, name); \
	static void name()

#endif
