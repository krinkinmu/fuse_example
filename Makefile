CC ?= gcc

FUSE_CFLAGS := $(shell pkg-config fuse3 --cflags)
FUSE_LFLAGS := $(shell pkg-config fuse3 --libs)
OPT := $(if $(DEBUG),,-O3)
CFLAGS := -g -Wall -Wextra -Werror -pedantic -Wframe-larger-than=1024 \
	-Wstack-usage=1024 -Wno-unknown-warning-option \
	-fno-omit-frame-pointer $(FUSE_CFLAGS) $(OPT) $(if $(DEBUG),-DDEBUG,)
LFLAGS := $(FUSE_LFLAGS)

AULSMFS_SRC	:= ./src
AULSMFS_INC	:= ./inc
AULSMFS_C	:= $(shell find $(AULSMFS_SRC) -name *.c)
AULSMFS_O	:= $(AULSMFS_C:.c=.o)
AULSMFS_D	:= $(AULSMFS_C:.c=.d)

all: aulsmfs

aulsmfs: $(AULSMFS_O)
	$(CC) $< $(LFLAGS) -o $@

$(AULSMFS_O): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) -MD -c $< -o $@

-include $(AULSMFS_D)

.PHONY: clean
clean:
	rm -f aulsmfs $(AULSMFS_O) $(AULSMFS_D)
