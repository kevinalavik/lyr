.SUFFIXES:

SUBDIRS := $(filter-out include,$(wildcard */))

.PHONY: all clean distclean
all:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C "$$dir" || exit 1; \
	done

distclean: clean

clean:
	@for dir in $(SUBDIRS); do \
		$(MAKE) -C "$$dir" clean 2>/dev/null || true; \
	done