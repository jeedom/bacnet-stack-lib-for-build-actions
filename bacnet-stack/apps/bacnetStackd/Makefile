# Makefile for bacnetStackd - Based on server-mini approach
# Use relative paths for portability
BACNET_ROOT := ../..
BACNET_SRC_DIR := $(BACNET_ROOT)/src

# Executable
TARGET = bacnetStackd
TARGET_BIN = $(TARGET)

# Source files - Compile only minimal objects needed
BACNET_OBJECT_DIR = $(BACNET_SRC_DIR)/bacnet/basic/object

SRC = bacnetStackd.c \
      trendlog_override.c \
      $(BACNET_OBJECT_DIR)/device.c \
      $(BACNET_OBJECT_DIR)/ai.c \
      $(BACNET_OBJECT_DIR)/ao.c \
      $(BACNET_OBJECT_DIR)/av.c \
      $(BACNET_OBJECT_DIR)/bi.c \
      $(BACNET_OBJECT_DIR)/bo.c \
      $(BACNET_OBJECT_DIR)/bv.c \
      $(BACNET_OBJECT_DIR)/ms-input.c \
      $(BACNET_OBJECT_DIR)/mso.c \
      $(BACNET_OBJECT_DIR)/msv.c \
      $(BACNET_OBJECT_DIR)/netport.c \
      $(BACNET_OBJECT_DIR)/nc.c \
      $(BACNET_OBJECT_DIR)/bitstring_value.c \
      $(BACNET_OBJECT_DIR)/blo.c \
      $(BACNET_OBJECT_DIR)/calendar.c \
      $(BACNET_OBJECT_DIR)/channel.c \
      $(BACNET_OBJECT_DIR)/color_object.c \
      $(BACNET_OBJECT_DIR)/color_temperature.c \
      $(BACNET_OBJECT_DIR)/command.c \
      $(BACNET_OBJECT_DIR)/csv.c \
      $(BACNET_OBJECT_DIR)/iv.c \
      $(BACNET_OBJECT_DIR)/lc.c \
      $(BACNET_OBJECT_DIR)/lo.c \
      $(BACNET_OBJECT_DIR)/lsp.c \
      $(BACNET_OBJECT_DIR)/lsz.c \
      $(BACNET_OBJECT_DIR)/osv.c \
      $(BACNET_OBJECT_DIR)/piv.c \
      $(BACNET_OBJECT_DIR)/program.c \
      $(BACNET_OBJECT_DIR)/time_value.c \
      $(BACNET_OBJECT_DIR)/trendlog.c \
      $(BACNET_OBJECT_DIR)/schedule.c \
      $(BACNET_OBJECT_DIR)/structured_view.c \
      $(BACNET_OBJECT_DIR)/acc.c \
      $(BACNET_OBJECT_DIR)/bacfile.c

# Paths - Now relative
PORTS_LINUX := $(BACNET_ROOT)/ports/linux
PORTS_POSIX := $(BACNET_ROOT)/ports/posix
BACNET_LIB_DIR := $(BACNET_ROOT)/apps/lib
BACNET_LIB_TARGET := $(BACNET_LIB_DIR)/libbacnet.a

# WORKAROUND: Compile datetime-init.c directly to get debug logs
SRC += $(PORTS_LINUX)/datetime-init.c

# Compiler flags
CFLAGS_COMMON := -std=gnu89 -Os -Wall -Wextra -pedantic \
                 -Wno-sign-conversion -Wno-conversion -Wno-sign-compare \
                 -DBACDL_BIP=1 -DINTRINSIC_REPORTING -DPRINT_ENABLED=1 \
                 -DBACAPP_ALL -DBACNET_TIME_MASTER \
                 -DBACNET_PROPERTY_LISTS=1 -DBACNET_PROTOCOL_REVISION=24 \
                 -DMAX_SCHEDULES=10 \
                 -DMAX_TREND_LOGS=50 \
                 -ffunction-sections -fdata-sections

INCLUDES := -I$(BACNET_SRC_DIR) -I$(PORTS_LINUX) -I$(PORTS_POSIX) \
            -I$(BACNET_ROOT) -I$(BACNET_ROOT)/include

CFLAGS := $(CFLAGS_COMMON) $(INCLUDES) $(PFLAGS)
LDFLAGS := -Wl,--gc-sections $(LFLAGS)
LDLIBS := $(BACNET_LIB_TARGET) -ljansson -lcurl -lpthread

# Objects
OBJS := $(SRC:.c=.o)

.PHONY: all clean

all: $(BACNET_LIB_TARGET) $(TARGET_BIN)

$(TARGET_BIN): $(OBJS) $(BACNET_LIB_TARGET)
	$(CC) $(PFLAGS) $(OBJS) $(LFLAGS) $(LDLIBS) -o $@
	@echo "Build complete: $@"
	@size $@ 2>/dev/null || true

$(BACNET_LIB_TARGET):
	@echo "Building BACnet library..."
	( cd $(BACNET_LIB_DIR) ; $(MAKE) clean ; $(MAKE) -s DEFINES="-DMAX_SCHEDULES=10 -DMAX_TREND_LOGS=50"  )

.c.o:
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	$(RM) -f $(TARGET_BIN) $(OBJS)

distclean: clean
	@true