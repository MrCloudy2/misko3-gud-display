# Host-side tools for M1. These build with the *native* compiler, not the ARM
# one: they run on the PC to generate and verify the test corpus that the
# target then decompresses.
#
#   make        - build both tools, fuzz the decoder, regenerate ../src/testdata.*
#   make fuzz   - correctness only
#   make gen    - regenerate the corpus only

CFLAGS  = -O2 -g -Wall -Wextra -fsanitize=address,undefined
LDLIBS  = -llz4

all: fuzz gen

fuzz_lz4: fuzz_lz4.c ../src/lz4_dec.c ../src/lz4_dec.h
	$(CC) $(CFLAGS) -o $@ fuzz_lz4.c ../src/lz4_dec.c $(LDLIBS)

gen_testdata: gen_testdata.c ../src/lz4_dec.c ../src/lz4_dec.h
	$(CC) $(CFLAGS) -o $@ gen_testdata.c ../src/lz4_dec.c $(LDLIBS)

# AddressSanitizer is on deliberately: it turns a one-byte overrun in the
# decoder into a loud failure instead of a silent pass.
fuzz: fuzz_lz4
	./fuzz_lz4

gen: gen_testdata
	./gen_testdata

clean:
	rm -f fuzz_lz4 gen_testdata

.PHONY: all fuzz gen clean
