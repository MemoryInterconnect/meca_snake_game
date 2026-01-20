CC = riscv64-linux-gnu-gcc
#CC = gcc
CFLAGS = -Wall -Wextra -O2 -static
TARGET = snake
SRC = snake.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
