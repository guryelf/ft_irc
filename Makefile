NAME		:= ircserv
TEST_NAME	:= unit_tests

CXX			:= c++
CXXFLAGS	:= -Wall -Wextra -Werror -std=c++98
CPPFLAGS	:= -Isrc -MMD -MP
LDFLAGS		:=

OBJ_DIR		:= obj

# IRC core: pure protocol logic, no system calls. Shared by the server and the unit tests.
IRC_SRCS	:= src/irc/Channel.cpp \
			   src/irc/Client.cpp \
			   src/irc/Core.cpp \
			   src/irc/IrcError.cpp \
			   src/irc/Message.cpp \
			   src/irc/Text.cpp \
			   src/irc/cmd/Channels.cpp \
			   src/irc/cmd/Messaging.cpp \
			   src/irc/cmd/Operator.cpp \
			   src/irc/cmd/Registration.cpp

# Network shell: sockets and the single poll() loop.
NET_SRCS	:= src/net/Reactor.cpp

SRCS		:= src/main.cpp \
			   src/Log.cpp \
			   $(IRC_SRCS) \
			   $(NET_SRCS)

TEST_SRCS	:= tests/unit/main.cpp \
			   tests/unit/test_channel.cpp \
			   tests/unit/test_channels.cpp \
			   tests/unit/test_core.cpp \
			   tests/unit/test_edges.cpp \
			   tests/unit/test_message.cpp \
			   tests/unit/test_operator.cpp \
			   tests/unit/test_registration.cpp \
			   tests/unit/test_text.cpp

FUZZ_NAME	:= core_fuzz
FUZZ_SRCS	:= tests/fuzz/core_fuzz.cpp

OBJS		:= $(SRCS:%.cpp=$(OBJ_DIR)/%.o)
IRC_OBJS	:= $(IRC_SRCS:%.cpp=$(OBJ_DIR)/%.o)
TEST_OBJS	:= $(TEST_SRCS:%.cpp=$(OBJ_DIR)/%.o)
FUZZ_OBJS	:= $(FUZZ_SRCS:%.cpp=$(OBJ_DIR)/%.o)
DEPS		:= $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(FUZZ_OBJS:.o=.d)

SANITIZE	:= -g -fno-omit-frame-pointer -fsanitize=address,undefined

all: $(NAME)

$(NAME): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(TEST_NAME): $(IRC_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IRC_OBJS) $(TEST_OBJS) -o $@

$(FUZZ_NAME): $(IRC_OBJS) $(FUZZ_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(IRC_OBJS) $(FUZZ_OBJS) -o $@

$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

test: $(TEST_NAME)
	./$(TEST_NAME)

integration: $(NAME)
	python3 tests/integration/irc_test.py ./$(NAME)

# Same server built with AddressSanitizer and UBSan, in its own object directory.
asan:
	$(MAKE) NAME=ircserv_asan OBJ_DIR=obj_asan \
		CXXFLAGS="$(CXXFLAGS) $(SANITIZE)" LDFLAGS="-fsanitize=address,undefined" all

# Random-input testing of the IRC core, with sanitizers. Usage of the
# binary: ./core_fuzz [iterations] [seed]
fuzz:
	$(MAKE) OBJ_DIR=obj_asan \
		CXXFLAGS="$(CXXFLAGS) $(SANITIZE)" LDFLAGS="-fsanitize=address,undefined" $(FUZZ_NAME)
	./$(FUZZ_NAME) 300000

clean:
	rm -rf $(OBJ_DIR) obj_asan

fclean: clean
	rm -f $(NAME) $(TEST_NAME) ircserv_asan $(FUZZ_NAME)

re: fclean
	$(MAKE) all

-include $(DEPS)

.PHONY: all clean fclean re test integration asan fuzz
