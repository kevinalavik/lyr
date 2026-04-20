.PHONY: sources
sources: $(KERNEL_DIR)/include/boot/axboot.h  $(ROOT_DIR)/axboot $(ROOT_DIR)/ovmf

$(KERNEL_DIR)/include/boot/axboot.h:
	@mkdir -p "$(dir $@)"
	./utils/fetch.sh -t file -o "$@" \
		https://raw.githubusercontent.com/piraterna/axboot/de828cc763521da01fhcfdd2a735ebbf64d749c81/include/proto/aurix.

$(ROOT_DIR)/axboot:
	@mkdir -p "$(dir $@)"
	./utils/fetch.sh -t git -o "$@" \
		https://github.com/piraterna/axboot

$(ROOT_DIR)/ovmf:
	@mkdir -p "$(dir $@)"
	./utils/fetch.sh -t git -o "$@" \
		https://github.com/piraterna/ovmf-bins