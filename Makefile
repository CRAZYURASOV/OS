CC      := gcc
CFLAGS  := -Wall -Wextra -pedantic -fPIC -O2
LDFLAGS :=

LIBNAME := libcaesar.so
TESTBIN := test_app

.PHONY: all install test clean

all: $(LIBNAME) $(TESTBIN)

$(LIBNAME): caesar.o
	$(CC) -shared -o $@ $^

caesar.o: caesar.c caesar.h
	$(CC) $(CFLAGS) -c caesar.c -o caesar.o

$(TESTBIN): test_app.o
	$(CC) -o $@ $^ -ldl

test_app.o: test_app.c
	$(CC) -Wall -Wextra -pedantic -O2 -c test_app.c -o test_app.o

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

clean:
	rm -f *.o $(LIBNAME) $(TESTBIN) encrypted.bin decrypted.txt