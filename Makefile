CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -I.
LDFLAGS := -lpthread

TARGET := nvme_reader
SRCS := main.c nvme_read.c
OBJS := $(SRCS:.c=.o)
TEST_TARGET := post_action_file_tester
TEST_SRCS := tests/post_action_file_tester.c nvme_read.c
TEST_OBJS := $(TEST_SRCS:.c=.o)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

%.o: %.c nvme_read.h
	$(CC) $(CFLAGS) -c $< -o $@

test: $(TEST_TARGET)
	bash tests/run_post_action_tests.sh

clean:
	rm -f $(OBJS) $(TARGET) $(TEST_OBJS) $(TEST_TARGET)
