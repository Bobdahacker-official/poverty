APPS = magenta ush hello cat udp http worker linuxhello linuxfork
APP_FILES = $(addprefix ../avalanche/build/user/,$(addsuffix .elf,$(APPS)))
.PHONY: all package
all:
	$(MAKE) -C ../avalanche magenta
package: all
	mkdir -p ../Images
	cp ../avalanche/build/magenta.iso ../Images/magenta.iso
	@set -eu; release_dir=$$(mktemp -d ../avalanche/build/magenta-release.XXXXXX); \
	trap 'rm -rf "$$release_dir"' EXIT; \
	python3 ../avalanche/tools/mkdisk.py "$$release_dir/magenta-data.img" $(APP_FILES) files/welcome.txt; \
	cp "$$release_dir/magenta-data.img" ../Images/magenta-data.img
