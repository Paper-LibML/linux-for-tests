CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -O2 -I.. -I../..
TARGET  := sched_ml_test

.PHONY: all clean run

all: $(TARGET)

$(TARGET): main.c ../include/fixpoint.h ../include/dtree.h ../model/model_weights.h
	$(CC) $(CFLAGS) -o $@ main.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
