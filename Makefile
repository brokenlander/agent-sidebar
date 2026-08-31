CC      ?= cc
CFLAGS  ?= -std=c17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wstrict-prototypes
LDFLAGS ?=
PREFIX  ?= $(HOME)/.local

SRC  := src/json.c src/agents.c src/render.c
MAIN := src/main.c
BIN  := agent-sidebar

.PHONY: all clean test sanitize install

all: $(BIN)

$(BIN): $(SRC) $(MAIN) src/json.h src/agents.h src/render.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(MAIN) $(LDFLAGS)

# Tests run under ASan+UBSan against synthetic fixtures *and* the real
# ~/.claude/sessions files, so malformed input on this machine is caught here.
test: tests/test_json.c $(SRC)
	$(CC) -std=c17 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -o /tmp/cs-test tests/test_json.c $(SRC)
	@/tmp/cs-test
	@command -v node >/dev/null && node tests/test_opencode_plugin.mjs || \
		echo "  SKIP  opencode producer tests (node not found)"

sanitize: $(SRC) $(MAIN)
	$(CC) -std=c17 -g -O1 -Wall -Wextra -fsanitize=address,undefined \
	      -o $(BIN)-asan $(SRC) $(MAIN)

install: $(BIN)
	install -d $(PREFIX)/bin
	install -m 755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(BIN)-asan /tmp/cs-test
