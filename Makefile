PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
PYTHON ?= python3

.PHONY: all themes server check test install clean
all: themes server
themes:
	npm run build
server:
	$(MAKE) -C server
check test: all
	$(MAKE) -C server test-platform
	$(PYTHON) tools/desktop/check.py
install:
	$(PYTHON) tools/desktop/install.py --prefix '$(PREFIX)' --libexecdir '$(LIBEXECDIR)' --destdir '$(DESTDIR)'
clean:
	$(MAKE) -C server clean
