CC      = gcc
LD      = ld
OBJCOPY = objcopy

EFIINC  = /usr/include/efi
EFILIB  = /usr/lib
EFICRT0 = $(EFILIB)/crt0-efi-x86_64.o
EFILDSC = $(EFILIB)/elf_x86_64_efi.lds

CFLAGS  = -I$(EFIINC) -I$(EFIINC)/x86_64 \
          -fpic -ffreestanding -fno-stack-protector \
          -fno-stack-check -fshort-wchar -mno-red-zone \
          -Wall -O2

LDFLAGS = -nostdlib -znocombreloc -T $(EFILDSC) \
          -shared -Bsymbolic -L$(EFILIB) \
          $(EFICRT0)

all: paint3.efi

paint3.o: paint3_efi.c
	$(CC) $(CFLAGS) -c -o $@ $<

paint3.so: paint3.o
	$(LD) $(LDFLAGS) -o $@ $^ -lefi -lgnuefi

paint3.efi: paint3.so
	$(OBJCOPY) \
	  -j .text -j .sdata -j .data -j .rodata \
	  -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc \
	  --target=efi-app-x86_64 $< $@

clean:
	rm -f *.o *.so *.efi
