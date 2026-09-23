# Builds the link daemon (mcdma-rpcd) and its helper library (libmcdma-rpc) into build/rpc.
# macOS links Apple's librdma (MCDMA supplies the provider); Linux links rdma-core's libibverbs.
UNAME := $(shell uname -s)
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
PREFIX ?= /usr/local
OUT ?= ../build/rpc

ifeq ($(UNAME),Darwin)
VERBS := -lrdma
HELPER := libmcdma-rpc.dylib
SHARED := -dynamiclib -install_name $(PREFIX)/lib/$(HELPER)
else
# shm_open lives in librt before glibc 2.34
VERBS := -libverbs -lrt
HELPER := libmcdma-rpc.so
SHARED := -shared -fPIC
endif

DAEMON := mcdma-rpcd.c rpcd_common.c rpcd_verbs.c rpcd_listen.c rpcd_connect.c

all: $(OUT)/mcdma-rpcd $(OUT)/$(HELPER)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/mcdma-rpcd: $(DAEMON) rpcd.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(DAEMON) $(VERBS) -lpthread

$(OUT)/$(HELPER): libmcdma_rpc.c mcdma_rpc.h | $(OUT)
	$(CC) $(CFLAGS) $(SHARED) -o $@ libmcdma_rpc.c

test: ../tests/test_rpc_helper.c ../tests/test_rpcd_socket.c libmcdma_rpc.c mcdma_rpc.h rpcd_common.c rpcd.h | $(OUT)
	$(CC) $(CFLAGS) -o $(OUT)/test-rpc-helper ../tests/test_rpc_helper.c libmcdma_rpc.c -lpthread
	$(OUT)/test-rpc-helper
	$(CC) $(CFLAGS) -o $(OUT)/test-rpcd-socket ../tests/test_rpcd_socket.c rpcd_common.c
	$(OUT)/test-rpcd-socket
	cd .. && python3 -B tests/test_rpcd_daemon.py

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 0755 $(OUT)/mcdma-rpcd $(DESTDIR)$(PREFIX)/bin/
	install -m 0644 $(OUT)/$(HELPER) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 mcdma_rpc.h $(DESTDIR)$(PREFIX)/include/

clean:
	rm -rf $(OUT)

.PHONY: all test install clean
