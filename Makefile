PLUGIN_NAME = exec-event-plugin.so
PLUGIN_SOURCES := exec-event-plugin.c
PLUGIN_TARGET_NAME = exec_event_plugin.so

DOVECOT_MODULES = /usr/lib/dovecot/modules
DOVECOT_INCLUDES = /usr/include/dovecot


.PHONY: all build install clean

all: build

build: ${PLUGIN_NAME} 

${PLUGIN_NAME}: ${PLUGIN_SOURCES}
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -std=gnu99 -fPIC -shared -Wall -I${DOVECOT_INCLUDES} -DHAVE_CONFIG_H $< -o $@

install: build
	install ${PLUGIN_NAME} ${DOVECOT_MODULES}/${PLUGIN_TARGET_NAME}

clean:
	$(RM) ${PLUGIN_NAME}
