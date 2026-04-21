CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -I.
LDFLAGS := -lpthread

TARGET := sfx_ctx_insight_analyze
SRCS := main.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
OBJS := $(SRCS:.c=.o)
LIB_TARGET := libinsight_api.a
LIB_SRCS := insight_api.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
TEST_TARGET := post_action_file_tester
TEST_SRCS := tests/post_action_file_tester.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
API_TEST_TARGET := insight_api_tester
API_TEST_SRCS := tests/insight_api_tester.c
API_TEST_OBJS := $(API_TEST_SRCS:.c=.o)
API_EXAMPLE_TARGET := insight_api_example
API_EXAMPLE_SRCS := examples/insight_api_example.c
API_EXAMPLE_OBJS := $(API_EXAMPLE_SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET) $(LIB_TARGET) $(API_EXAMPLE_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(LIB_TARGET): $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

$(API_TEST_TARGET): $(API_TEST_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_TEST_OBJS) $(LIB_TARGET) $(LDFLAGS)

$(API_EXAMPLE_TARGET): $(API_EXAMPLE_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_EXAMPLE_OBJS) $(LIB_TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	bash tests/run_post_action_tests.sh

clean:
	rm -f $(OBJS) $(TARGET) $(LIB_OBJS) $(LIB_TARGET) $(TEST_OBJS) $(TEST_TARGET) \
		$(API_TEST_OBJS) $(API_TEST_TARGET) $(API_EXAMPLE_OBJS) $(API_EXAMPLE_TARGET)
