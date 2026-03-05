CC      := gcc
CFLAGS1 := -Wall -Wextra -pedantic -fPIC -O2
CFLAGS2 := -Wall -Wextra -pedantic -O2 -pthread

LIBNAME := libcaesar.so
TESTBIN := test_app
COPYBIN := secure_copy

.PHONY: all install test test_copy clean

all: $(LIBNAME) $(TESTBIN) $(COPYBIN)

# --------- libcaesar.so (Задание 1) ----------
$(LIBNAME): caesar.o
	$(CC) -shared -o $@ $^

caesar.o: caesar.c caesar.h
	$(CC) $(CFLAGS1) -c caesar.c -o caesar.o

# --------- test_app (Задание 1, dlopen) ------
$(TESTBIN): test_app.o
	$(CC) -o $@ $^ -ldl

test_app.o: test_app.c
	$(CC) -Wall -Wextra -pedantic -O2 -c test_app.c -o test_app.o

# --------- secure_copy (Задание 2) -----------
$(COPYBIN): secure_copy.o $(LIBNAME)
	$(CC) -o $@ secure_copy.o -L. -lcaesar -Wl,-rpath,'$$ORIGIN' -pthread

secure_copy.o: secure_copy.c caesar.h
	$(CC) $(CFLAGS2) -c secure_copy.c -o secure_copy.o

install: $(LIBNAME)
	sudo cp $(LIBNAME) /usr/local/lib/
	sudo ldconfig

test: all
	@echo "Creating sample input.txt (if not exists)..."
	@test -f input.txt || echo "Hello XOR library! 123" > input.txt
	@echo "Encrypting: input.txt -> encrypted.bin"
	./$(TESTBIN) ./$(LIBNAME) 42 input.txt encrypted.bin
	@echo "Decrypting: encrypted.bin -> decrypted.txt"
	./$(TESTBIN) ./$(LIBNAME) 42 encrypted.bin decrypted.txt
	@echo "Diff (should be empty):"
	@diff -u input.txt decrypted.txt || true
	@echo "Done."

test_copy: all
	@echo "Creating sample input.txt (if not exists)..."
	@test -f input.txt || echo "Hello secure_copy! 123" > input.txt
	@echo "Encrypting via secure_copy: input.txt -> enc.bin"
	./$(COPYBIN) input.txt enc.bin 42
	@echo "Decrypting via secure_copy: enc.bin -> dec.txt"
	./$(COPYBIN) enc.bin dec.txt 42
	@echo "Diff (should be empty):"
	@diff -u input.txt dec.txt || true
	@echo "Done."

clean:
	rm -f *.o $(LIBNAME) $(TESTBIN) $(COPYBIN) encrypted.bin decrypted.txt enc.bin dec.txt