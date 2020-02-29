
# Makefile for MiniOMP testing

APPNAME ?= main
SRCNAME ?= main.c

MFLAGS := -fno-stack-protector
CFLAGS := -O2 -Wall -Wextra $(MFLAGS)
# use LFLAGS="-fopenmp" to build with the original OpenMP runtime library
LFLAGS := miniomp.o -lpthread

SFLAGS := -masm=intel -Wno-all
SFLAGS += -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables

.PHONY: clean all

all: $(APPNAME)

clean:
	rm -f $(APPNAME) $(SRCNAME:.c=.o) $(SRCNAME:.c=.s) miniomp.o miniomp.s

%.s: %.c
	$(CC) $(CFLAGS) -S $< -o $@ $(SFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ -fopenmp

$(APPNAME): $(SRCNAME:.c=.o) miniomp.o
	$(CC) $(CFLAGS) -s $< -o $@ $(LFLAGS)

