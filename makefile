# Simple Makefile for Lab 4 Scheduler
#make        -compile, generate ./group50_scheduler
#make clean  -remove ./group50_scheduler

CC = gcc
CFLAGS = -Wall -Wextra -std=c11

TARGET = group50_scheduler
SRCS = group50_scheduler.c queue.c
HEADERS = queue.h

all: $(TARGET)

$(TARGET): $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(TARGET)