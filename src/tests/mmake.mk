ifeq ($(GNU),1)
SUBDIRS		:= ciul \
                   nic \
                   ip \
                   citools \
                   driver \
                   ef_vi \
                   onload


OTHER_SUBDIRS	:= tweaks \
                   syscalls

ifdef BROKEN
OTHER_SUBDIRS	+= cplane
endif

ifeq ($(ONLOAD_ONLY),1)
SUBDIRS		:= ef_vi \
                   onload
endif
endif

DRIVER_SUBDIRS	:= driver

ifeq ($(FREEBSD),1)
SUBDIRS         := ip
DRIVER_SUBDIRS	:=
OTHER_SUBDIRS	:=
endif

ifeq ($(MACOSX),1)
SUBDIRS         := ip
DRIVER_SUBDIRS	:=
OTHER_SUBDIRS	:=
endif

ifeq ($(SOLARIS),1)
SUBDIRS       	:= solaris nic ip
DRIVER_SUBDIRS	:=
OTHER_SUBDIRS	:=
endif

all:
	+@$(MakeSubdirs)

clean:
	@$(MakeClean)

