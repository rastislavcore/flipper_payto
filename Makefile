CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I.

test: tests/test_payto
	./tests/test_payto

tests/test_payto: tests/test_payto.c payto.c payto.h
	$(CC) $(CFLAGS) tests/test_payto.c payto.c -o $@

clean:
	rm -f tests/test_payto
