CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11 -Iinclude -Isrc
LDFLAGS := -lpthread -lm

LIB_SRCS := insight_api.c insight_api_json.c insight_metalog.c nvme_read.c \
            post_action.c post_action_stats.c post_action_latency.c
LIB_OBJS := $(addprefix src/,$(LIB_SRCS:.c=.o))
LIB_PIC_OBJS := $(LIB_OBJS:.o=.pic.o)

LIB_TARGET := libinsight_api.a
SHARED_LIB_TARGET := libinsight_api.so

API_TEST_TARGET := insight_api_tester
API_TEST_OBJS := tests/insight_api_tester.o

JSON_TEST_TARGET := insight_api_json_tester
JSON_TEST_OBJS := tests/insight_api_json_tester.o src/insight_api_json.o

API_EXAMPLE_TARGET := insight_api_example
API_EXAMPLE_OBJS := examples/insight_api_example.o

API_SESSION_EXAMPLE_TARGET := insight_api_session_example
API_SESSION_EXAMPLE_OBJS := examples/insight_api_session_example.o

.PHONY: all clean test shared examples

all: $(LIB_TARGET) $(SHARED_LIB_TARGET) examples

examples: $(API_EXAMPLE_TARGET) $(API_SESSION_EXAMPLE_TARGET)

shared: $(SHARED_LIB_TARGET)

test: $(API_TEST_TARGET) $(JSON_TEST_TARGET)
	bash tests/run_tests.sh

$(LIB_TARGET): $(LIB_OBJS)
	ar rcs $@ $(LIB_OBJS)

$(SHARED_LIB_TARGET): $(LIB_PIC_OBJS)
	$(CC) -shared -Wl,-soname,$@ -o $@ $(LIB_PIC_OBJS) $(LDFLAGS)

$(API_TEST_TARGET): $(API_TEST_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_TEST_OBJS) $(LIB_TARGET) $(LDFLAGS)

$(JSON_TEST_TARGET): $(JSON_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(JSON_TEST_OBJS) $(LDFLAGS)

$(API_EXAMPLE_TARGET): $(API_EXAMPLE_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_EXAMPLE_OBJS) $(LIB_TARGET) $(LDFLAGS)

$(API_SESSION_EXAMPLE_TARGET): $(API_SESSION_EXAMPLE_OBJS) $(LIB_TARGET)
	$(CC) $(CFLAGS) -o $@ $(API_SESSION_EXAMPLE_OBJS) $(LIB_TARGET) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/%.pic.o: src/%.c
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

tests/%.o: tests/%.c
	$(CC) $(CFLAGS) -c $< -o $@

examples/%.o: examples/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(LIB_OBJS) $(LIB_PIC_OBJS) $(LIB_TARGET) $(SHARED_LIB_TARGET) \
		$(API_TEST_OBJS) $(API_TEST_TARGET) \
		$(JSON_TEST_OBJS) $(JSON_TEST_TARGET) \
		$(API_EXAMPLE_OBJS) $(API_EXAMPLE_TARGET) \
		$(API_SESSION_EXAMPLE_OBJS) $(API_SESSION_EXAMPLE_TARGET)
