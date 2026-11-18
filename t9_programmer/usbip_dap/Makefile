CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -I. -pthread
LDFLAGS ?= -pthread

SRCS = usbip_server.c DAP.c swo_capture.c ../stlink/stlink_swd.c
OBJS = $(SRCS:.c=.o)
TARGET = t9-cmsis-dap

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c DAP_config.h DAP.h usbip_proto.h stream_ring.h swo_capture.h usbip_session.inc ../common/resource_lock.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
