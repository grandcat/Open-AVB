CC=gcc
OPT=-O2 -g
CFLAGS=$(OPT) -Wall -Wextra -Wno-parentheses
INCFLAGS=-I ../../lib/igb -I../../daemons/mrpd -I../common
LDLIBS=-ligb -lpci -lrt -lm -pthread -lpcap -lsndfile
LDFLAGS=-L ../../lib/igb

all: simple_talker simple_listener

simple_talker: simple_talker.o ../common/avb.o ../common/talker_mrp_client.o

simple_talker.o: simple_talker.c
	$(CC) $(CFLAGS) $(INCFLAGS) -c simple_talker.c

../common/talker_mrp_client.o: ../common/talker_mrp_client.c ../common/talker_mrp_client.h
	make -C ../common/ talker_mrp_client.o

../common/avb.o: ../common/avb.c ../common/avb.h
	make -C ../common/ avb.o

simple_listener: simple_listener.o ../common/listener_mrp_client.o

simple_listener.o: simple_listener.c
	$(CC) $(CFLAGS) $(INCFLAGS) -c simple_listener.c

../common/listener_mrp_client.o: ../common/listener_mrp_client.c ../common/listener_mrp_client.h
	make -C ../common/ listener_mrp_client.o

%: %.o
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	$(RM) simple_talker simple_listener
	$(RM) `find . -name "*~" -o -name "*.[oa]" -o -name "\#*\#" -o -name TAGS -o -name core -o -name "*.orig"`
