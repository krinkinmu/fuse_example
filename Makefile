CC	?= gcc

FUSE_CFLAGS	:= $(shell pkg-config fuse3 --cflags)
FUSE_LFLAGS	:= $(shell pkg-config fuse3 --libs)

MKFS_CFLAGS	:=
MKFS_LFLAGS	:=

CFLAGS	:= -D_GNU_SOURCE\
	-g -Wall -Wextra -Werror -pedantic -Wframe-larger-than=1024 \
	-Wstack-usage=1024 -Wno-unknown-warning-option \
	-fno-omit-frame-pointer $(if $(DEBUG),-DDEBUG,-O3)

AULSMFS_FUSE	:= ./fuse
AULSMFS_MKFS	:= ./mkfs
AULSMFS_INC	:= ./inc

AULSMFS_FUSE_SRC	:= $(shell find $(AULSMFS_FUSE) -name '*.c')
AULSMFS_FUSE_OBJ	:= $(AULSMFS_FUSE_SRC:.c=.o)
AULSMFS_FUSE_DEP	:= $(AULSMFS_FUSE_SRC:.c=.d)

AULSMFS_MKFS_SRC	:= $(shell find $(AULSMFS_MKFS) -name '*.c')
AULSMFS_MKFS_OBJ	:= $(AULSMFS_MKFS_SRC:.c=.o)
AULSMFS_MKFS_DEP	:= $(AULSMFS_MKFS_SRC:.c=.d)

OBJ	:= $(AULSMFS_FUSE_OBJ) $(AULSMFS_MKFS_OBJ)
DEP	:= $(AULSMFS_FUSE_DEP) $(AULSMFS_MKFS_DEP)

all: aulsmfs.fuse aulsmfs.mkfs

aulsmfs.fuse: $(AULSMFS_FUSE_OBJ)
	$(CC) $^ $(FUSE_LFLAGS) -o $@

aulsmfs.mkfs: $(AULSMFS_MKFS_OBJ)
	$(CC) $^ $(MKFS_LFLAGS) -o $@

$(AULSMFS_FUSE_OBJ): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) $(FUSE_CFLAGS) -MD -c $< -o $@

$(AULSMFS_MKFS_OBJ): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) $(MKFS_CFLAGS) -MD -c $< -o $@

-include $(DEP)

.PHONY: clean
clean:
	rm -f aulsmfs.fuse aulsmfs.mkfs $(OBJ) $(DEP)
