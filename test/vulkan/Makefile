CC      ?= gcc
CFLAGS  ?= -O1 -Wall
LDFLAGS ?= -lvulkan -pthread

all: vkmem_test vkmem_stress

vkmem_test: vkmem_test.c
	$(CC) $(CFLAGS) $< -o $@ -lvulkan

vkmem_stress: vkmem_stress.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f vkmem_test vkmem_stress
