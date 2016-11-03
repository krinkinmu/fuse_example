CC	?= gcc
AR	?= ar

FUSE_CFLAGS	:= $(shell pkg-config fuse3 --cflags)
FUSE_LFLAGS	:= $(shell pkg-config fuse3 --libs)
MKFS_CFLAGS	:=
MKFS_LFLAGS	:=
LIB_CFLAGS	:=

CFLAGS	:= -D_GNU_SOURCE\
	-g -Wall -Wextra -Werror -pedantic -Wframe-larger-than=1024 \
	-Wstack-usage=1024 -Wno-unknown-warning-option \
	-fno-omit-frame-pointer $(if $(DEBUG),-DDEBUG,-O3)

LFLAGS	:= -L. -laulsmfs

AULSMFS_FUSE	:= ./fuse
AULSMFS_MKFS	:= ./mkfs
AULSMFS_LIB	:= ./lib
AULSMFS_INC	:= ./inc

AULSMFS_FUSE_SRC	= $(shell find $(AULSMFS_FUSE) -name '*.c')
AULSMFS_FUSE_OBJ	= $(AULSMFS_FUSE_SRC:.c=.o)
AULSMFS_FUSE_DEP	= $(AULSMFS_FUSE_SRC:.c=.d)

AULSMFS_MKFS_SRC	= $(shell find $(AULSMFS_MKFS) -name '*.c')
AULSMFS_MKFS_OBJ	= $(AULSMFS_MKFS_SRC:.c=.o)
AULSMFS_MKFS_DEP	= $(AULSMFS_MKFS_SRC:.c=.d)

AULSMFS_LIB_SRC	= $(shell find $(AULSMFS_LIB) -name '*.c')
AULSMFS_LIB_OBJ	= $(AULSMFS_LIB_SRC:.c=.o)
AULSMFS_LIB_DEP	= $(AULSMFS_LIB_SRC:.c=.d)
AULSMFS_LIB_GEN	= $(AULSMFS_LIB)/crc64_table.h

OBJ	:= $(AULSMFS_FUSE_OBJ) $(AULSMFS_MKFS_OBJ) $(AULSMFS_LIB_OBJ)
DEP	:= $(AULSMFS_FUSE_DEP) $(AULSMFS_MKFS_DEP) $(AULSMFS_LIB_DEP)
GEN	:= $(AULSMFS_FUSE_GEN) $(AULSMFS_MKFS_GEN) $(AULSMFS_LIB_GEN)

all: gen aulsmfs.fuse aulsmfs.mkfs

aulsmfs.fuse: $(AULSMFS_FUSE_OBJ) libaulsmfs.a
	$(CC) $^ $(LFLAGS) $(FUSE_LFLAGS) -o $@

aulsmfs.mkfs: $(AULSMFS_MKFS_OBJ) libaulsmfs.a
	$(CC) $^ $(LFLAGS) $(MKFS_LFLAGS) -o $@

libaulsmfs.a: $(AULSMFS_LIB_OBJ)
	$(AR) rcs $@ $(AULSMFS_LIB_OBJ)

$(AULSMFS_FUSE_OBJ): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) $(FUSE_CFLAGS) -MD -c $< -o $@

$(AULSMFS_MKFS_OBJ): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) $(MKFS_CFLAGS) -MD -c $< -o $@

$(AULSMFS_LIB_OBJ): %.o: %.c
	$(CC) -I$(AULSMFS_INC) $(CFLAGS) $(LIB_CFLAGS) -MD -c $< -o $@

-include $(DEP)

.PHONY: clean gen
gen: $(GEN)

$(AULSMFS_LIB)/crc64_table.h: gen_crc64_table.py
	python gen_crc64_table.py > $@

clean:
	rm -f aulsmfs.fuse aulsmfs.mkfs libaulsmfs.a $(OBJ) $(DEP) $(GEN)
