CC ?= gcc
CFLAGS ?= -O2 -Wall -I.
LDFLAGS ?= 

SRCS = usbip_server.c DAP.c
OBJS = $(SRCS:.c=.o)
TARGET = t9-cmsis-dap

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c DAP_config.h DAP.h usbip_proto.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
