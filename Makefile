CC := gcc
CFLAGS := -Wall -Wextra -pedantic -std=c11
LDFLAGS := -lpthread

TARGET := nvme_reader
SRCS := main.c nvme_read.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c nvme_read.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
