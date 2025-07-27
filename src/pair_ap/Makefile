ifeq ($(CC),cc)
CC=$(lastword $(subst /, ,$(shell readlink -f `which cc`)))
endif

ifeq ($(findstring gcc,$(CC)),gcc)
CFLAGS  += -Wno-stringop-truncation
LDFLAGS += -s
else
CFLAGS += -fno-temp-file
endif

PLATFORM ?= $(firstword $(subst -, ,$(CC)))
HOST ?= $(word 2, $(subst -, ,$(CC)))

SRC        = .
CORE       = bin/clipair_ap-$(HOST)
BUILDDIR   = $(dir $(CORE))$(HOST)/$(PLATFORM)
LIB		   = lib/$(HOST)/$(PLATFORM)/libpair_ap.a
EXECUTABLE = $(CORE)-$(PLATFORM)
DEFINES	   = -DCONFIG_GCRYPT -DCONFIG_OPENSSL

CFLAGS  += -Wall -fPIC -ggdb -O2 $(DEFINES) -fdata-sections -ffunction-sections 

vpath %.c $(SRC)

INCLUDE = -I$(SRC) 

SOURCES =  pair_fruit.c pair_homekit.c pair-tlv.c pair.c
	
OBJECTS = $(SOURCES:%.c=$(BUILDDIR)/%.o) 

# all: lib $(EXECUTABLE)
all: lib
lib: directory $(LIB)
directory:
	@mkdir -p lib/$(HOST)/$(PLATFORM)	
	@mkdir -p $(BUILDDIR)		

$(EXECUTABLE): $(BUILDDIR)/pair_ap.o  $(LIB)
	$(CC) $^ $(CFLAGS) $(LDFLAGS) -o $@
ifeq ($(HOST),macos)
	rm -f $(CORE)
	lipo -create -output $(CORE) $$(ls $(CORE)* | grep -v '\-static')
endif

$(LIB): $(OBJECTS)
	$(AR) -rcs $@ $^

$(BUILDDIR)/%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

cleanlib:
	rm -f $(BUILDDIR)/*.o $(LIB) 

clean: cleanlib
	rm -f $(EXECUTABLE) $(CORE)