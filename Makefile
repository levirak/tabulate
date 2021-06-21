program_name = $(notdir $(CURDIR))
libraries    = libpulse
source_dir   = src
build_dir    = build

o = .o
d = .mk
e =

CPPDIRS = -iquote $(build_dir) -iquote .
LDDIRS  =
LDLIBS  = -lpthread $(shell pkg-config --libs $(libraries))

override CPPFLAGS := $(CPPFLAGS) $(CPPDIRS)
override LDFLAGS  := $(LDFLAGS) $(LDDIRS)
base_CFLAGS       := -std=gnu11 $(CFLAGS) $(shell pkg-config --cflags $(libraries))

sources = $(wildcard $(source_dir)/*.c)
objects = $(patsubst $(source_dir)/%.c,$(build_dir)/%$o,$(sources))
deps    = $(patsubst $(source_dir)/%.c,$(build_dir)/%$d,$(sources))
target  = $(build_dir)/$(program_name)$e

# The Target Build
all: CFLAGS = -g -Og -Wall -Wextra $(base_CFLAGS) -fsanitize=address,undefined,leak
all: $(target)

debug: CFLAGS = -g -Wall -Wextra $(base_CFLAGS) -fsanitize=address,undefined,leak
debug: $(target)

release: CFLAGS = -O2 -Wall -Wextra -DNDEBUG $(base_CFLAGS)
release: $(target)

-include $(deps)
$(deps): $(build_dir)/%$d: $(source_dir)/%.c
	@echo "  CPP   $@"
	@mkdir -p $(dir $@)
	@$(CPP) $(CPPFLAGS) $< -MM -MG -MT $(@:$d=$o) -MF $@

$(target): $(objects)
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(objects): %$o: # dependencies provided by deps
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $< $(LDFLAGS) $(LDLIBS)


test_sourcs = $(wildcard tests/*_tests.c)
tests       = $(patsubst %.c,%,$(test_sourcs))

$(tests): %: %.c $(filter-out $(build_dir)/main$o,$(objects))
	@echo "  CC    $@"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -iquote $(source_dir) -o $@ $^ $(LDFLAGS) $(LDLIBS)

.PHONY: tests
tests: $(tests)
	@sh ./tests/runtests.sh


.PHONY: clean cleaner
clean:
	@echo "  RM    $(build_dir)"
	@rm -rf $(build_dir)
	@echo "  RM    $(tests)"
	@rm -rf $(tests)

cleaner: clean
	@echo "  RM    tags"
	@rm -f tags

.PHONY: tags
tags:
	@echo "  CTAGS $(source_dir)"
	@ctags -R $(source_dir)
