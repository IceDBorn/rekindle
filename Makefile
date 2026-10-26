CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Icommon
LDFLAGS = -shared -static -s -Wl,--exclude-all-symbols

COMMON = $(wildcard common/*.c)
REKINDLE = $(wildcard rekindle/*.c)
SNAPSHOT = $(wildcard snapshot/*.c)
HEADERS = $(wildcard common/*.h rekindle/*.h snapshot/*.h)

all: rekindle.dll

rekindle.dll: $(COMMON) $(REKINDLE) $(SNAPSHOT) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(COMMON) $(REKINDLE) $(SNAPSHOT) -lbcrypt -lws2_32

clean:
	rm -f rekindle.dll

.PHONY: all clean
