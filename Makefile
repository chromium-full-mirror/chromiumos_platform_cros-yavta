include common.mk

CFLAGS := -I$(SRC)/include $(CFLAGS)

LDLIBS	:= -pthread -lXv -lXext -lX11 -lv4lconvert -lvpx -levent -levent_core -levent_extra -lrt -ljpeg

all: CC_BINARY(yavta) CC_BINARY(yavta_server)

CC_BINARY(yavta): \
  device.o \
  display.o \
  filter.o \
  option.o \
  process.o \
  remote_client.o \
  remote.o \
  save.o \
  third_party/libjpeg/jcparam.o \
  third_party/libjpeg/jdatasrc.o \
  third_party/libvpx/libmkv/EbmlWriter.o \
  third_party/libvpx/webm.o \
  util.o \
  v4l2_json.o \
  webui.o \
  yavta.o

CC_BINARY(yavta_server): remote_server.o remote.o

clean: CLEAN(CC_BINARY(yavta)) CLEAN(CC_BINARY(yavta_server))
