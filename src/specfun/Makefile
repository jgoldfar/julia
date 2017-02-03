JULIAHOME := $(abspath ../..)
BUILDDIR := .
include $(JULIAHOME)/Make.inc

override CFLAGS_add =
override CPPFLAGS_add =

ifeq ($(USECLANG),1)
  override CFLAGS_add += -fno-builtin
endif

ifeq ($(USEGCC),1)
  override CFLAGS_add += -fno-gnu89-inline -std=c99
endif

ifeq ($(USE_SYSTEM_LIBM),0)
  override CPPFLAGS_add += -DUSE_OPENLIBM -I$(build_includedir) -I$(build_includedir)/openlibm
endif

override CFLAGS += $(JCFLAGS) -O3 -std=c99
ifeq ($(USEICC),1)
  override CFLAGS += -fp-model precise
endif

ifeq ($(OS),WINNT)
  override CFLAGS_add += -nodefaultlibs
else
  override CFLAGS_add += -fPIC
endif

SRCS := e_rem_pio2.c e_rem_pio2f.c k_rem_pio2.c
OBJS := $(addprefix $(BUILDDIR)/,$(patsubst %.c,%.o,$(SRCS)))

default: $(JULIA_BUILD_MODE)

all: release debug
release: $(BUILDDIR)/libspecfun.a
debug: $(BUILDDIR)/libspecfun-debug.a

$(BUILDDIR)/%.o: $(BUILDDIR)/%.c
	$(CC) $(CPPFLAGS) $(CPPFLAGS_add) $(CFLAGS_add) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/libspecfun.a: $(OBJS)
	$(AR) -rcs libspecfun.a $(OBJS)

$(BUILDDIR)/libspecfun-debug.a: $(OBJS)
	$(AR) -rcs libspecfun-debug.a $(OBJS)

clean:
	cd $(BUILDDIR) && rm $(OBJS) *.a
