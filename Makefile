CC      := gcc
CFLAGS1 := -Wall -Wextra -pedantic -fPIC -O2
CFLAGS2 := -Wall -Wextra -pedantic -O2 -pthread

LIBNAME := libcaesar.so
TESTBIN := test_app
COPYBIN := secure_copy

.PHONY: all install test test_copy clean demo_files

all: $(LIBNAME) $(TESTBIN) $(COPYBIN)

$(LIBNAME): caesar.o
	$(CC) -shared -o $@ $^

caesar.o: caesar.c caesar.h
	$(CC) $(CFLAGS1) -c caesar.c -o caesar.o

$(TESTBIN): test_app.o
	$(CC) -o $@ $^ -ldl

test_app.o: test_app.c
	$(CC) -Wall -Wextra -pedantic -O2 -c test_app.c -o test_app.o

$(COPYBIN): secure_copy.o $(LIBNAME)
	$(CC) -o $@ secure_copy.o -L. -lcaesar -Wl,-rpath,'$$ORIGIN' -pthread

secure_copy.o: secure_copy.c caesar.h
	$(CC) $(CFLAGS2) -c secure_copy.c -o secure_copy.o

install: $(LIBNAME)
	sudo cp $(LIBNAME) /usr/local/lib/
	sudo ldconfig

test: all
	@test -f input.txt || echo "Hello XOR library! 123" > input.txt
	./$(TESTBIN) ./$(LIBNAME) 42 input.txt encrypted.bin
	./$(TESTBIN) ./$(LIBNAME) 42 encrypted.bin decrypted.txt
	diff -u input.txt decrypted.txt || true

demo_files:
	@mkdir -p demo_in
	@printf 'alpha\n' > demo_in/f1.txt
	@printf 'beta\n' > demo_in/f2.txt
	@printf 'gamma\n' > demo_in/f3.txt
	@printf 'delta\n' > demo_in/f4.txt
	@printf 'epsilon\n' > demo_in/f5.txt

test_copy: all demo_files
	@rm -rf outdir restored
	./$(COPYBIN) demo_in/f1.txt demo_in/f2.txt demo_in/f3.txt demo_in/f4.txt demo_in/f5.txt outdir 42
	./$(COPYBIN) outdir/f1.txt outdir/f2.txt outdir/f3.txt outdir/f4.txt outdir/f5.txt restored 42
	diff -u demo_in/f1.txt restored/f1.txt
	diff -u demo_in/f2.txt restored/f2.txt
	diff -u demo_in/f3.txt restored/f3.txt
	diff -u demo_in/f4.txt restored/f4.txt
	diff -u demo_in/f5.txt restored/f5.txt

clean:
	rm -f *.o $(LIBNAME) $(TESTBIN) $(COPYBIN) encrypted.bin decrypted.txt log.txt
	rm -rf demo_in outdir restored