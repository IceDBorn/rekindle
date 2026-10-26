CC = x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Icommon
LDFLAGS = -shared -static -s -Wl,--exclude-all-symbols

COMMON = $(wildcard common/*.c)
REKINDLE = $(wildcard rekindle/*.c)
SNAPSHOT = $(wildcard snapshot/*.c)
HEADERS = $(wildcard common/*.h rekindle/*.h snapshot/*.h)

all: rekindle.dll rekindle_snapshot.dll

rekindle.dll: $(COMMON) $(REKINDLE) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(COMMON) $(REKINDLE) -lbcrypt -lws2_32

rekindle_snapshot.dll: $(COMMON) $(SNAPSHOT) $(HEADERS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(COMMON) $(SNAPSHOT)

clean:
	rm -f rekindle.dll rekindle_snapshot.dll

.PHONY: all clean
