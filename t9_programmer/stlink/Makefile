CC ?= gcc
CFLAGS ?= -O2 -Wall -I.
TARGET = plink-stlink

SRCS = stlink_server.c stlink_swd.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c stlink_swd.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
