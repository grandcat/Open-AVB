CC=gcc
OPT=-O2 -g
CFLAGS=$(OPT) -Wall -Wextra -Wno-parentheses
INCFLAGS=
LDLIBS=-lrt -lm -pthread
LDFLAGS=

all: parse.o 

parse: parse.o

parse.o: parse.c parse.h
	$(CC) $(CFLAGS) -c parse.c

%: %.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	$(RM) `find . -name "*~" -o -name "*.[oa]" -o -name "\#*\#" -o -name TAGS -o -name core -o -name "*.orig"`
