CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -I.
LDFLAGS := -lpthread

TARGET := sfx_ctx_insight_analyze
SRCS := main.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
OBJS := $(SRCS:.c=.o)
TEST_TARGET := post_action_file_tester
TEST_SRCS := tests/post_action_file_tester.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
TEST_OBJS := $(TEST_SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

%.o: %.c nvme_read.h post_action.h post_action_stats.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	bash tests/run_post_action_tests.sh

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_OBJS) $(TEST_TARGET)
