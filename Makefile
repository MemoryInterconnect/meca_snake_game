CC = riscv64-linux-gnu-gcc
CFLAGS = -Wall -Wextra -O2 -static
LDFLAGS = -static

# Path to RISC-V static libraries (set this to your ncurses install path)
LIBDIR ?= /usr/riscv64-linux-gnu/lib
INCDIR ?= /usr/riscv64-linux-gnu/include

LIBS = -lncursesw -ltinfo

TARGET = snake
SRC = snake.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -L$(LIBDIR) -o $@ $< $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
