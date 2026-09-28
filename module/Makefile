include ../../Makefile.defs
auto_gen=
NAME=txms.so
DEFS+=-Werror -I$(TXMS_ROOT)/include -I$(LIBTXMS_ROOT)/include $(shell pkg-config --cflags libcurl json-c gmime-3.0)
LIBS+=$(TXMS_BUILD)/libtxms_gateway.a $(TXMS_BUILD)/libtxms/libtxms.a $(shell pkg-config --libs libcurl json-c gmime-3.0)
include ../../Makefile.modules
