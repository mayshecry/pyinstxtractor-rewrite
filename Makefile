CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS ?= -lz

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin

SRCS = src/pyinstxtractor.c src/main.c
OBJS = $(SRCS:.c=.o)
TARGET = pyinstxtractor
HEADER = src/pyinstxtractor.h

# Sanitizer flags for `make asan`
SANFLAGS = -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
           -fno-omit-frame-pointer

.PHONY: all clean install uninstall check smoke asan

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c $(HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET)-asan

# Build a sanitizer-instrumented binary.
$(TARGET)-asan: $(SRCS) $(HEADER)
	$(CC) $(SANFLAGS) -o $@ $(SRCS) $(LDFLAGS)

asan: $(TARGET)-asan

# End-to-end smoke test. Builds a throwaway PyInstaller executable unless
# PYINST_EXE points at an existing one, then verifies every extracted .pyc.
smoke: $(TARGET)
	@test/smoke.sh "$(PYINST_EXE)"

# Full test suite: end-to-end extraction plus path-safety, both driven through
# the ASan/UBSan build so memory errors and leaks are fatal.
check: $(TARGET)-asan
	@echo "==> smoke test (ASan/UBSan)"
	@PYINST_BIN="$(CURDIR)/$(TARGET)-asan" \
	 ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 test/smoke.sh "$(PYINST_EXE)"
	@echo "==> path-safety test (ASan/UBSan)"
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 test/pathsafety.sh \
	 "$(CURDIR)/$(TARGET)-asan"
	@echo "==> encrypted PYZ test (ASan/UBSan)"
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 test/encrypted.sh \
	 "$(CURDIR)/$(TARGET)-asan"
	@echo "==> check passed"

install: $(TARGET)
	install -d $(BINDIR)
	install -m 0755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)
