CC = gcc
OPT = -O2 -g
CFLAGS = $(OPT) -Wall -Wextra -Wno-parentheses

all: avb.o ../../daemons/common/parse.o talker_mrp_client.o listener_mrp_client.o

#VPATH = ../../daemons/common

avb.o: avb.c avb.h
	$(CC) $(CFLAGS) -I../../lib/igb -c avb.c

parse.o: ../../daemons/common/parse.c ../../daemons/common/parse.h
	make -C ../../daemons/common/ parse.c

talker_mrp_client.o: talker_mrp_client.c talker_mrp_client.h
	$(CC) $(CFLAGS) -I../../daemons/mrpd -c talker_mrp_client.c

listener_mrp_client.o: listener_mrp_client.c listener_mrp_client.h
	$(CC) $(CFLAGS) -I../../daemons/mrpd -I../../daemons/common -c listener_mrp_client.c

clean:
	$(RM)  avb.o talker_mrp_client.o listener_mrp_client.o
	$(RM) `find . -name "*~" -o -name "*.[oa]" -o -name "\#*\#" -o -name TAGS -o -name core -o -name "*.orig"`
