# Build aesdsocket (native or cross)
CC       ?= $(CROSS_COMPILE)gcc
CFLAGS   ?= -Wall -Wextra -Werror -pedantic -std=c99
LDFLAGS  ?=
TARGET    = aesdsocket

.PHONY: all default clean

all default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	$(RM) $(TARGET)

