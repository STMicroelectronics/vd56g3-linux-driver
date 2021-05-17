CROSS_COMPILE ?=
.DEFAULT_GOAL := all

CC	:= $(CROSS_COMPILE)gcc
CFLAGS ?= -W -Wall
LDFLAGS	?=
LIBS	:=

%.o : %.cpp
	$(CC) $(CFLAGS) -c -o $@ $<

mpane_capture: mpane_capture.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

update_v4lctrl: update_v4lctrl.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

all: mpane_capture update_v4lctrl

clean:
	-rm -f *.o
	-rm -f mpane_capture
	-rm -f update_v4lctrl