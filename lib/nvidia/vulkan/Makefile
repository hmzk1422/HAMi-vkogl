CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC -fvisibility=hidden -D_GNU_SOURCE
LDFLAGS ?= -shared -Wl,-z,defs -pthread

libVkLayer_hami_vgpu.so: hami_vk_layer.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f libVkLayer_hami_vgpu.so
