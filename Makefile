CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -I.
LDFLAGS := -lpthread

TARGET := sfx_ctx_insight_analyze
SRCS := main.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c insight_metalog.c
OBJS := $(SRCS:.c=.o)
LIB_TARGET := libinsight_api.a
SHARED_LIB_TARGET := libinsight_api.so
LIB_SRCS := insight_api.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c insight_metalog.c
LIB_OBJS := $(LIB_SRCS:.c=.o)
SHARED_LIB_OBJS := $(LIB_SRCS:.c=.pic.o)
TEST_TARGET := post_action_file_tester
TEST_SRCS := tests/post_action_file_tester.c nvme_read.c post_action.c post_action_stats.c post_action_latency.c
TEST_OBJS := $(TEST_SRCS:.c=.o)
API_TEST_TARGET := insight_api_tester
API_TEST_SRCS := tests/insight_api_tester.c
API_TEST_OBJS := $(API_TEST_SRCS:.c=.o)
API_EXAMPLE_TARGET := insight_api_example
API_EXAMPLE_SRCS := examples/insight_api_example.c
API_EXAMPLE_OBJS := $(API_EXAMPLE_SRCS:.c=.o)
DYN_API_DEMO_TARGET := insight_api_dynamic_demo
DYN_API_DEMO_SRCS := examples/insight_api_dynamic_demo.c
DYN_API_DEMO_OBJS := $(DYN_API_DEMO_SRCS:.c=.o)

.PHONY: all clean test shared

all: $(TARGET) $(LIB_TARGET) $(SHARED_LIB_TARGET) $(API_EXAMPLE_TARGET) $(DYN_API_DEMO_TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(LIB_TARGET): $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

$(SHARED_LIB_TARGET): $(SHARED_LIB_OBJS)
	$(CC) -shared -Wl,-soname,$@ -o $@ $(SHARED_LIB_OBJS) $(LDFLAGS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(LDFLAGS)

$(API_TEST_TARGET): $(API_TEST_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_TEST_OBJS) $(LIB_TARGET) $(LDFLAGS)

$(API_EXAMPLE_TARGET): $(API_EXAMPLE_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_EXAMPLE_OBJS) $(LIB_TARGET) $(LDFLAGS)

$(DYN_API_DEMO_TARGET): $(DYN_API_DEMO_OBJS) $(SHARED_LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(DYN_API_DEMO_OBJS) -L. -linsight_api $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.pic.o: %.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

shared: $(SHARED_LIB_TARGET)

test: $(TEST_TARGET)
	bash tests/run_post_action_tests.sh

clean:
	rm -f $(OBJS) $(TARGET) $(LIB_OBJS) $(LIB_TARGET) $(SHARED_LIB_OBJS) $(SHARED_LIB_TARGET) \
		$(TEST_OBJS) $(TEST_TARGET) $(API_TEST_OBJS) $(API_TEST_TARGET) \
		$(API_EXAMPLE_OBJS) $(API_EXAMPLE_TARGET) $(DYN_API_DEMO_OBJS) $(DYN_API_DEMO_TARGET)
