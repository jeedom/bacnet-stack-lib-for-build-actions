#Makefile to build BACnet Stack Client (bacnetStackc)
# Aligned with bacnetStackd configuration

# Paths - Relative
BACNET_ROOT := ../..
BACNET_SRC_DIR := $(BACNET_ROOT)/src
PORTS_LINUX := $(BACNET_ROOT)/ports/linux
PORTS_POSIX := $(BACNET_ROOT)/ports/posix
BACNET_LIB_DIR := $(BACNET_ROOT)/apps/lib
BACNET_LIB_TARGET := $(BACNET_LIB_DIR)/libbacnet.a

# Executable
TARGET = bacnetStackc
TARGET_BIN = $(TARGET)

# Compiler flags - Similar to server
CFLAGS_COMMON := -std=gnu89 -Os -Wall -Wextra -pedantic \
                 -Wno-sign-conversion -Wno-conversion -Wno-sign-compare \
                 -DBACDL_BIP=1 -DPRINT_ENABLED=1 \
                 -DBACAPP_ALL \
                 -DBACNET_PROPERTY_LISTS=1 -DBACNET_PROTOCOL_REVISION=24 \
                 -DBACNET_IP_BROADCAST_USE_INADDR_ANY=1 \
                 -ffunction-sections -fdata-sections

INCLUDES := -I$(BACNET_SRC_DIR) -I$(PORTS_LINUX) -I$(PORTS_POSIX) \
            -I$(BACNET_ROOT) -I$(BACNET_ROOT)/include

CFLAGS := $(CFLAGS_COMMON) $(INCLUDES) $(PFLAGS)
LDFLAGS := -Wl,--gc-sections -pthread $(LFLAGS)
LDLIBS := $(BACNET_LIB_TARGET) -ljansson -lm

# Source files - AJOUT DU STUB DCC
SRC = bacnetStackc.c dcc-stub.c

# Objects
OBJS := $(SRC:.c=.o)

.PHONY: all clean

all: $(BACNET_LIB_TARGET) $(TARGET_BIN)

$(TARGET_BIN): $(OBJS) $(BACNET_LIB_TARGET)
	$(CC) $(PFLAGS) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@
	@echo "Build complete: $@"
	@size $@ 2>/dev/null || true

$(BACNET_LIB_TARGET):
	@echo "Building BACnet library..."
	( cd $(BACNET_LIB_DIR) ; $(MAKE) clean ; \
	  $(MAKE) -s BACNET_DEFINES="-DBACNET_IP_BROADCAST_USE_INADDR_ANY=1" )

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	$(RM) -f $(TARGET_BIN) $(OBJS)
	rm -f /tmp/bacnetStackc.sock

distclean: clean
	@true

# Additional targets
install: $(TARGET_BIN)
	install -m 755 $(TARGET_BIN) /usr/local/bin/

uninstall:
	rm -f /usr/local/bin/$(TARGET_BIN)

run: $(TARGET_BIN)
	./$(TARGET_BIN)