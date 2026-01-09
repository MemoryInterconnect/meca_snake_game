#CC = riscv64-linux-gnu-gcc
CC = gcc
#CFLAGS = -Wall -Wextra -O2 -static
CFLAGS = -Wall -Wextra -O2
#LDFLAGS = -static
LDFLAGS =

LIBS = -lncursesw -ltinfo

TARGET = snake
SRC = snake.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -I$(INCDIR) -L$(LIBDIR) -o $@ $< $(LIBS) $(LDFLAGS)

clean:
	rm -f $(TARGET)
