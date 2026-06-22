# SPDX-License-Identifier: GPL-2.0
CFLAGS += -static -O3 -Wl,-no-as-needed -Wall $(KHDR_INCLUDES)
LDLIBS += -lm

TEST_GEN_PROGS = dmabuf-heap dmabuf-heap-bench

include ../lib.mk
