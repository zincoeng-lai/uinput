CC=arm-linux-gnueabihf-gcc
CFLAGS=-Wall -O2 -std=c99

TARGET=mca_uinput
SRC=uinput_cli.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)
