SUBDIRS = include src tests

dist_man1_MANS = man/aicli.1

EXTRA_DIST = README.md docs/design.md scripts/qa.sh .clang-format .clang-tidy

.PHONY: qa lint format format-check

qa:
	./scripts/qa.sh check

lint: qa

format:
	./scripts/qa.sh fix

format-check:
	./scripts/qa.sh check
