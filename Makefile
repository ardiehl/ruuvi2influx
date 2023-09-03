# specify what libraries should be static linked (e.g. mparser is not available on RedHat 8)

# paho (mqtt) static or dynamic
PAHOSTATIC     = 0

# libcurl static or dynamic, currently (08/2023) raspberry as well as Fedora 38
# have versions installed that does not support websockets
CURLSTATIC = 1


TARGETS = ruuvimqtt2influx

# OS dependend executables
WGET           = wget -q --show-progress
TAR            = tar
MAKEDIR        = mkdir -p
RM             = rm -f
RMRF           = rm -rf
COPY           = cp
ARCH           = $(shell uname -m && mkdir -p obj-`uname -m`/influxdb-post && mkdir -p obj-`uname -m`/libmbus/mbus)
SUDO           = sudo
INSTALLDIR     = /usr/local
INSTALLDIR_BIN = $(INSTALLDIR)/bin
INSTALLDIR_CFG = $(INSTALLDIR)/etc
INSTALLDIR_SYS = $(INSTALLDIR)/lib/systemd/system
SYSTEMD_RELOAD = systemctl daemon-reload
XZUNPACK       = xz -d -c

ALLTARGETS = $(TARGETS:=$(TGT))

CPPFLAGS = -fPIE -g0 -Os -Wall -g -Imqtt$(TGT)/include  -DSML_NO_UUID_LIB

# auto generate dependency files
CPPFLAGS += -MMD

.PHONY: default all clean info Debug cleanDebug

default: $(ALLTARGETS)
all: default
Debug: all
cleanDebug: clean

LIBS = -lm -lpthread
OBJDIR       = obj-$(ARCH)$(TGT)
SOURCES      = $(wildcard *.c influxdb-post/*.c libmbus/mbus/*.c *.cpp)
OBJECTS      = $(filter %.o, $(patsubst %.c, $(OBJDIR)/%.o, $(SOURCES)) $(patsubst %.cpp, $(OBJDIR)/%.o, $(SOURCES)))
MAINOBJS     = $(patsubst %, $(OBJDIR)/%.o,$(TARGETS))
LINKOBJECTS  = $(filter-out $(MAINOBJS), $(OBJECTS))
#MAINOBJS    = $(patsubst %, $(OBJDIR)/%.o,$(ALLTARGETS))
DEPS         = $(OBJECTS:.o=.d)

ifeq ($(PAHOSTATIC),1)
#MQTTLIBDIR  = $(shell ./getmqttlibdir)
ifeq ($(TGT),-gx)
MQTTLIBDIR   = mqtt-gx/lib
else
MQTTLIBDIR   = mqtt/lib64
endif
MQTTLIB      = libpaho-mqtt3c.a
MQTTLIBP     = $(MQTTLIBDIR)/$(MQTTLIB)
LIBS       += -L./$(MQTTLIBDIR) -l:$(MQTTLIB)
else
LIBS         += -lpaho-mqtt3c
endif


ifeq ($(CURLSTATIC),1)
CURLVERSION  = 8.2.1
CURLSRCFILE  = curl-$(CURLVERSION).tar.xz
CURLSRC      = https://github.com/curl/curl/releases/download/curl-8_2_1/$(CURLSRCFILE)
CURLDIR      = curl$(ARCH)$(TGT)
CURLTAR      = $(CURLDIR)/$(CURLSRCFILE)
CURLMAKEDIR  = $(CURLDIR)/curl-$(CURLVERSION)
CURLMAKE     = $(CURLMAKEDIR)/Makefile
CURLLIB      = $(CURLMAKEDIR)/lib/.libs/libcurl.a
LIBS         += $(CURLLIB) -lz -lssl -lcrypto -lzstd
CPPFLAGS     += -I$(CURLMAKEDIR)/include -DCURL_STATIC
else
LIBS          += -lcurl
endif



# include dependencies if they exist
-include $(DEPS)


# ------------------------ libmodbus static ----------------------------------

ifeq ($(MODBUSSTATIC),1)

$(MODBUSTAR):
	@$(MAKEDIR) $(MODBUSDIR)
	@echo "Downloading $(MODBUSSRC)"
	@cd $(MODBUSDIR); $(WGET) $(MODBUSSRC)


$(MODBUSMAKE):        $(MODBUSTAR)
	@echo "unpacking $(MODBUSSRCFILE)"
	@cd $(MODBUSDIR); $(TAR) x --gunzip < $(MODBUSSRCFILE);
	@echo "Generating Makefile"
	@cd $(MODBUSMAKEDIR); ./configure -q
	@echo

$(MODBUSLIB): $(MODBUSMAKE)
	@echo "Compiling modbus"
	@$(MAKE) -s -C $(MODBUSMAKEDIR)
	@echo "Generating $(MODBUSLIB)"
	@$(AR) r $(MODBUSLIB) $(MODBUSMAKEDIR)/src/.libs/*.o
	@$(MAKE) -s -C $(MODBUSMAKEDIR) clean

endif


ifeq ($(PAHOSTATIC),1)
$(MQTTLIBP):
	@cd paho; ./buildmqtt || exit 1; cd ..
endif

# ------------------------ libmcurl static -----------------------------------
ifeq ($(CURLSTATIC),1)

$(CURLTAR):
	@$(MAKEDIR) $(CURLDIR)
	@echo "Downloading $(CURLSRC)"
	@cd $(CURLDIR); $(WGET) $(CURLSRC)

$(CURLMAKE):        $(CURLTAR)
	@echo "unpacking $(CURLSRCFILE)"
	@cd $(CURLDIR); $(XZUNPACK) $(CURLSRCFILE) | $(TAR) xv
	@echo "Generating Makefile"
	@cd $(CURLMAKEDIR); ./configure --without-psl --disable-file --disable-ldap --disable-ldaps --disable-tftp --disable-dict --without-libidn2 --with-openssl --enable-websockets  --disable-ftp --disable-rtsp --disable-telnet --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt --disable-manual --disable-ntlm --disable-unix-sockets --disable-cookies --without-brotli
	@echo

$(CURLLIB): $(CURLMAKE)
	@echo "Compiling modbus"
	@$(MAKE) -s -C $(CURLMAKEDIR)

endif



$(OBJDIR)/%.o: %.c  $(MQTTLIBP) $(MUPARSERLIB) $(CURLLIB)
	@echo -n "compiling $< to $@ "
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


$(OBJDIR)/%.o: %.cpp $(MQTTLIBP) $(MUPARSERLIB) $(CURLLIB)
	@echo -n "compiling $< to $@ "
	@$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@
	@echo ""


.PRECIOUS: $(TARGETS) $(ALLOBJECTS)

$(ALLTARGETS): $(OBJECTS) $(SMLLIBP) $(MQTTLIBP) $(MUPARSERLIB) $(CURLLIB)
	@echo -n "linking $@ "
	$(CXX) $(OBJDIR)/$(patsubst %$(TGT),%,$@).o $(LINKOBJECTS) -Wall $(LIBS) -o $@
	@echo ""


build: clean all

install: $(ALLTARGETS)
	@echo "Installing in $(INSTALLDIR)"
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_BIN)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_CFG)
	$(SUDO) $(MAKEDIR) $(INSTALLDIR_SYS)
	$(SUDO) $(COPY) $(TARGETS) $(INSTALLDIR_BIN)
	$(SUDO) $(COPY) ruuvimqtt2influx.service $(INSTALLDIR_SYS)
	$(SUDO) $(SYSTEMD_RELOAD)

clean:
	@$(RM) $(OBJECTS) $(TARGETS) $(DEPS) $(MQTTLIBP)
	@echo "cleaned"

distclean:	clean
ifeq ($(MUPARSERSTATIC),1)
	@$(RMRF) $(MUPARSERDIR)
endif
ifeq ($(CURLSTATIC),1)
	@$(RMRF) $(CURLDIR)
endif
	rm -rf $(OBJDIR)
	@echo "cleaned static build dirs"

info:
	@echo "       TARGETS: $(TARGETS)"
	@echo "    ALLTARGETS: $(ALLTARGETS)"
	@echo "       SOURCES: $(SOURCES)"
	@echo "       OBJECTS: $(OBJECTS)"
	@echo "   LINKOBJECTS: $(LINKOBJECTS)"
	@echo "      MAINOBJS: $(MAINOBJS)"
	@echo "          DEPS: $(DEPS)"
	@echo "    CC/CPP/CXX: $(CC)/$(CPP)/$(CXX)"
	@echo "        CFLAGS: $(CFLAGS)"
	@echo "      CPPFLAGS: $(CPPFLAGS)"
	@echo "      CXXFLAGS: $(CXXFLAGS)"
	@echo "          LIBS: $(LIBS)"
	@echo "    MQTTLIBDIR: $(MQTTLIBDIR)"
	@echo "       MQTTLIB: $(MQTTLIB)"
	@echo "    CURLSTATIC: $(CURLSTATIC)"
	@echo "       CURLLIB: $(CURLLIB)"
	@echo "       CURLDIR: $(CURLDIR)"
	@echo "       CURLTAR: $(CURLTAR)"
	@echo "   INSTALLDIRS: $(INSTALLDIR_BIN) $(INSTALLDIR_CFG) $(INSTALLDIR_SYS)"
	@echo "$(notdir $(MUPARSERSRC))"
