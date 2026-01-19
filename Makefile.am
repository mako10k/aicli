SUBDIRS = include src tests

EXTRA_DIST = README.md docs/design.md scripts/qa.sh .clang-format .clang-tidy

.PHONY: qa lint format format-check

qa:
	./scripts/qa.sh check

lint: qa

format:
	./scripts/qa.sh fix

format-check:
	./scripts/qa.sh check
