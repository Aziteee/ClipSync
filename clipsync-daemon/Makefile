CC ?= aarch64-linux-android33-clang
CFLAGS = -Wall -Wextra -O2 -static
TARGET = clipsyncd
SRCS = clipsyncd.c

all: $(TARGET)

$(TARGET): $(SRCS) protocol.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
