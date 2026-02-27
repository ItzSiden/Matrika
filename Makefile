# মাতৃকা (Matrika) — Makefile

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99
TARGET  = matrika
SRC     = src/matrika.c

ifdef CROSS
  CC     = x86_64-w64-mingw32-gcc
  TARGET = matrika.exe
endif

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo ">> মাতৃকা সফলভাবে তৈরি হয়েছে: ./$(TARGET)"

clean:
	rm -f matrika matrika.exe

install: matrika
	cp matrika /usr/local/bin/
	@echo ">> ইনস্টল সম্পন্ন: /usr/local/bin/matrika"