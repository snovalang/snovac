# snovac — Snovalang compiler, C11.
#
# Namespaces are directories under src/ plus an identifier prefix:
#   src/base     sn_arena_  sn_diag_  sn_intern_
#   src/lex      sn_lex_    sn_tok_
#   src/parse    sn_parse_
#   src/ast      sn_list_   sn_dump_
#   src/sema     sn_scope_  sn_pkggraph_  sn_type_  sn_resolve_
#                sn_builtin_  sn_check_  sn_borrow_
#   src/eval     sn_eval_  sn_rt_  sn_async_  sn_pulsar_  sn_socket_
#   src/bc       sn_chunk_  sn_bcunit_  sn_emit_
#   src/native   sn_native_  sn_target_
#   src/driver   sn_driver_  sn_project_  sn_cmd_
#
# `make compdb` writes compile_commands.json for clangd from the flags below.
# compile_flags.txt mirrors those flags for a checkout that has not been built.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wconversion -Wno-sign-conversion
INCLUDES = -Isrc/base -Isrc/lex -Isrc/parse -Isrc/ast -Isrc/sema \
           -Isrc/eval -Isrc/bc -Isrc/native -Isrc/driver
BUILD   ?= build

# OS detection for `install`/`uninstall`: native Windows `make` (and
# MSYS2/Git Bash, which still inherit OS=Windows_NT from the environment)
# need a .exe suffix and a PowerShell-based PATH setup instead of the
# POSIX shell one used for Linux/macOS.
ifeq ($(OS),Windows_NT)
  EXE := .exe
  EXTRA_LIBS := -lws2_32
else
  EXE :=
  EXTRA_LIBS := -lpthread
endif

BIN      = $(BUILD)/snl$(EXE)

SRCS = src/driver/main.c src/driver/driver_utils.c src/driver/project.c \
       src/driver/cmd_check.c src/driver/cmd_lex_parse.c src/driver/cmd_run.c \
       src/driver/cmd_build.c src/driver/cmd_tidy.c src/driver/cmd_get.c \
       src/driver/cmd_emit_snbc.c \
       src/native/target.c src/native/native_backend.c \
       src/eval/pulsar.c src/eval/async.c \
       src/ast/dump.c src/ast/ast.c \
       src/lex/lex.c src/lex/lex_token.c src/lex/lex_literal.c \
       src/parse/parse.c src/parse/parse_type.c src/parse/parse_expr.c \
       src/parse/parse_primary.c src/parse/parse_stmt.c \
       src/parse/parse_decl.c src/parse/parse_decl_parts.c src/parse/parse_ptr.c \
       src/eval/eval.c src/eval/eval_expr.c src/eval/eval_stmt.c \
       src/eval/eval_string.c src/eval/rt_mem.c src/eval/rt_ptr.c src/eval/rt_defer.c \
       src/eval/socket_abi.c src/eval/native_dispatch.c \
       src/base/diag.c src/base/arena.c src/base/intern.c \
       src/sema/symbol.c src/sema/package.c src/sema/types.c src/sema/resolve.c \
       src/sema/builtins.c src/sema/check.c src/sema/check_ptr.c \
       src/sema/borrow.c src/sema/borrow_expr.c src/sema/borrow_flow.c \
       src/sema/borrow_task.c \
       src/bc/snbc.c src/bc/emit_bc.c
OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

# intern.c/symbol.c/package.c/types.c have no CLI surface yet (P2.1/P2.2/P2.3)
# — exercised directly by standalone C test binaries instead of through
# $(BIN). See tests/test_symbol.c, tests/test_package.c, tests/test_types.c.
TEST_SYMBOL_BIN = $(BUILD)/test_symbol$(EXE)
TEST_SNBC_BIN = $(BUILD)/test_snbc$(EXE)
TEST_PACKAGE_BIN = $(BUILD)/test_package$(EXE)
TEST_PACKAGE_OBJS = $(BUILD)/base/arena.o $(BUILD)/base/diag.o $(BUILD)/base/intern.o \
                     $(BUILD)/sema/symbol.o $(BUILD)/sema/package.o $(BUILD)/ast/ast.o \
                     $(BUILD)/lex/lex.o $(BUILD)/lex/lex_token.o $(BUILD)/lex/lex_literal.o \
                     $(BUILD)/driver/driver_utils.o
TEST_TYPES_BIN = $(BUILD)/test_types$(EXE)
TEST_TYPES_OBJS = $(BUILD)/base/arena.o $(BUILD)/base/intern.o $(BUILD)/sema/symbol.o \
                   $(BUILD)/sema/types.o
TEST_RESOLVE_BIN = $(BUILD)/test_resolve$(EXE)
TEST_RESOLVE_OBJS = $(BUILD)/base/arena.o $(BUILD)/base/diag.o $(BUILD)/base/intern.o \
                     $(BUILD)/sema/symbol.o $(BUILD)/sema/package.o $(BUILD)/sema/types.o \
                     $(BUILD)/sema/resolve.o $(BUILD)/ast/ast.o \
                     $(BUILD)/lex/lex.o $(BUILD)/lex/lex_token.o $(BUILD)/lex/lex_literal.o \
                     $(BUILD)/parse/parse.o $(BUILD)/parse/parse_type.o $(BUILD)/parse/parse_expr.o \
                     $(BUILD)/parse/parse_primary.o $(BUILD)/parse/parse_stmt.o \
                     $(BUILD)/parse/parse_decl.o $(BUILD)/parse/parse_decl_parts.o \
                     $(BUILD)/parse/parse_ptr.o $(BUILD)/driver/driver_utils.o
TEST_CHECK_BIN = $(BUILD)/test_check$(EXE)
TEST_CHECK_OBJS = $(TEST_RESOLVE_OBJS) $(BUILD)/sema/builtins.o $(BUILD)/sema/check.o \
                  $(BUILD)/sema/check_ptr.o $(BUILD)/sema/borrow.o $(BUILD)/sema/borrow_expr.o \
                  $(BUILD)/sema/borrow_flow.o $(BUILD)/sema/borrow_task.o

RT_SRCS = $(filter-out src/driver/main.c,$(SRCS))
RT_OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(RT_SRCS))
LIB_RT  = $(BUILD)/libsnovart.a

# Default install prefix (~/.snova on Unix, %USERPROFILE%/.snova on Windows)
ifeq ($(OS),Windows_NT)
  PREFIX  ?= $(USERPROFILE)/.snova
else
  PREFIX  ?= $(HOME)/.snova
endif
BINDIR  ?= $(PREFIX)/bin
LIBDIR  ?= $(PREFIX)/lib
INCDIR  ?= $(PREFIX)/include

# snova-std lives as a sibling directory of snovac/ in the repo. It's
# optional: if it isn't checked out, install just skips it (SNOVA_STD_DIR
# or the project's own .snovalang/deps can supply it another way).
STD_SRC_DIR := ../snova-std/src
STD_INSTALL_DIR ?= $(HOME)/.snovalang/std/src

.PHONY: all clean test unit conformance install uninstall installer-windows compdb

all: $(BIN) $(LIB_RT) compile_commands.json

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EXTRA_LIBS)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Copy-Item '$@' '$(BUILD)/snovac$(EXE)' -Force"
else
	@cp $@ $(BUILD)/snovac$(EXE)
endif

$(LIB_RT): $(RT_OBJS)
	ar rcs $@ $(RT_OBJS)

# Installs the toolchain binary as `snl` into BINDIR, its runtime static lib
# + headers (needed by `snl build --runtime`, which shells out to $(CC) again
# at run time) into LIBDIR/INCDIR, snova-std (if present) into
# STD_INSTALL_DIR, and wires BINDIR onto PATH for every common shell:
#   - bash   (~/.bashrc and ~/.bash_profile)
#   - zsh    (~/.zshrc)
#   - fish   (~/.config/fish/config.fish)
#   - PowerShell (User PATH env var + $PROFILE), on Windows
# Each is idempotent, so re-running `make install` is safe.
install: $(BIN) $(LIB_RT)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/install_windows.ps1 -Prefix "$(PREFIX)" -BinDir "$(BINDIR)" -LibDir "$(LIBDIR)" -IncDir "$(INCDIR)" -Bin "$(BIN)" -LibRt "$(LIB_RT)"
else
	@mkdir -p $(BINDIR) $(LIBDIR) $(INCDIR)
	install -m 755 $(BIN) $(BINDIR)/snl$(EXE)
	@echo "✓ Installed snl to $(BINDIR)/snl$(EXE)"
	install -m 644 $(LIB_RT) $(LIBDIR)/libsnovart.a
	find src -name '*.h' -type f -exec install -m 644 {} $(INCDIR)/ \;
	@echo "✓ Installed runtime lib + headers to $(LIBDIR), $(INCDIR)"
	@if [ -d $(STD_SRC_DIR) ]; then \
		mkdir -p "$(STD_INSTALL_DIR)"; \
		cp -R $(STD_SRC_DIR)/. "$(STD_INSTALL_DIR)/"; \
		echo "✓ Installed snova-std to $(STD_INSTALL_DIR)"; \
	fi
	@sh scripts/install_path.sh "$(BINDIR)"
endif

uninstall:
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/uninstall_windows.ps1 -Prefix "$(PREFIX)" -BinDir "$(BINDIR)" -LibDir "$(LIBDIR)" -IncDir "$(INCDIR)"
else
	rm -f $(BINDIR)/snl$(EXE) $(BINDIR)/snovac$(EXE) $(BINDIR)/sncli$(EXE)
	rm -f $(LIBDIR)/libsnovart.a
	rm -f $(addprefix $(INCDIR)/,$(notdir $(shell find src -name '*.h' -type f)))
	@echo "✓ Removed snl from $(BINDIR)/snl$(EXE) (and its runtime lib/headers)"
	@echo "Note: PATH entries added by 'make install' in shell rc files /"
	@echo "the PowerShell profile are left untouched; remove them manually if desired."
	@echo "Note: snova-std installed to $(STD_INSTALL_DIR) is left untouched."
endif

installer-windows: $(BIN) $(LIB_RT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File installer/windows/build_installer.ps1

$(BUILD)/%.o: src/%.c | $(BUILD)
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "New-Item -ItemType Directory -Force -Path '$(subst /,\,$(dir $@))' | Out-Null"
else
	@mkdir -p $(dir $@)
endif
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -MMD -MP -c -o $@ $<

$(BUILD):
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (-not (Test-Path '$(BUILD)')) { New-Item -ItemType Directory -Path '$(BUILD)' | Out-Null }"
else
	mkdir -p $(BUILD)
endif

$(TEST_SYMBOL_BIN): tests/test_symbol.c $(BUILD)/base/arena.o $(BUILD)/base/intern.o $(BUILD)/sema/symbol.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_symbol.c \
	    $(BUILD)/base/arena.o $(BUILD)/base/intern.o $(BUILD)/sema/symbol.o

$(TEST_SNBC_BIN): tests/test_snbc.c $(BUILD)/bc/snbc.o | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_snbc.c \
	    $(BUILD)/bc/snbc.o

$(TEST_PACKAGE_BIN): tests/test_package.c $(TEST_PACKAGE_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_package.c $(TEST_PACKAGE_OBJS)

$(TEST_TYPES_BIN): tests/test_types.c $(TEST_TYPES_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_types.c $(TEST_TYPES_OBJS)

$(TEST_RESOLVE_BIN): tests/test_resolve.c $(TEST_RESOLVE_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_resolve.c $(TEST_RESOLVE_OBJS)

$(TEST_CHECK_BIN): tests/test_check.c $(TEST_CHECK_OBJS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_check.c $(TEST_CHECK_OBJS)

# Assertions for the lexer decisions derived from the corpus, plus the
# standalone symbol-table, package-graph, type-representation, resolver and
# checker unit tests, plus the little-endian SnBC writer.
unit: $(BIN) $(TEST_SYMBOL_BIN) $(TEST_PACKAGE_BIN) $(TEST_TYPES_BIN) $(TEST_RESOLVE_BIN) $(TEST_CHECK_BIN) $(TEST_SNBC_BIN)
ifeq ($(OS),Windows_NT)
	@$(TEST_SYMBOL_BIN)
	@$(TEST_PACKAGE_BIN)
	@$(TEST_TYPES_BIN)
	@$(TEST_RESOLVE_BIN)
	@$(TEST_CHECK_BIN)
	@$(TEST_SNBC_BIN)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (Get-Command sh -ErrorAction SilentlyContinue) { sh tests/run.sh $(BIN); if ($$LASTEXITCODE -ne 0) { exit $$LASTEXITCODE }; sh tests/battery.sh $(BIN) } elseif (Test-Path 'C:\Program Files\Git\bin\sh.exe') { & 'C:\Program Files\Git\bin\sh.exe' tests/run.sh $(BIN); if ($$LASTEXITCODE -ne 0) { exit $$LASTEXITCODE }; & 'C:\Program Files\Git\bin\sh.exe' tests/battery.sh $(BIN) } else { Write-Host 'Note: tests/run.sh and tests/battery.sh skipped (requires bash/sh shell)' }"
else
	@sh tests/run.sh $(BIN)
	@sh tests/battery.sh $(BIN)
	@./$(TEST_SYMBOL_BIN)
	@./$(TEST_PACKAGE_BIN)
	@./$(TEST_TYPES_BIN)
	@./$(TEST_RESOLVE_BIN)
	@./$(TEST_CHECK_BIN)
	@./$(TEST_SNBC_BIN)
endif

# Lexes every .snl in the repository and reports coverage.
conformance: $(BIN)
	@sh scripts/snovac-conformance.sh $(BIN)

test: unit

clean:
ifeq ($(OS),Windows_NT)
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "if (Test-Path '$(BUILD)') { Remove-Item -Path '$(BUILD)' -Recurse -Force }"
else
	rm -rf $(BUILD)
endif
	rm -f compile_commands.json

compdb: compile_commands.json

compile_commands.json: $(SRCS) tests/test_symbol.c tests/test_package.c tests/test_types.c tests/test_resolve.c tests/test_check.c tests/test_snbc.c scripts/gen_compile_commands.py
ifeq ($(OS),Windows_NT)
	@python scripts/gen_compile_commands.py "$(CURDIR)" "$(CC)" "$(CPPFLAGS)" "$(CFLAGS)" "$(WARN)" "$(INCLUDES)"
else
	@python3 scripts/gen_compile_commands.py "$(CURDIR)" "$(CC)" "$(CPPFLAGS)" "$(CFLAGS)" "$(WARN)" "$(INCLUDES)"
endif

-include $(DEPS)
