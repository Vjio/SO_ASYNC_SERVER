CC = gcc
CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG
CFLAGS = -Wall -g
LDLIBS = -laio

.PHONY: all build clean

build: all

all: aws

aws: aws.o sock_util.o http_parser.o

aws.o: aws.c utils/sock_util.h utils/debug.h utils/util.h http-parser/http_parser.h aws.h

http_parser.o: http-parser/http_parser.c http-parser/http_parser.h
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c -o $@ $<

sock_util.o: utils/sock_util.c utils/sock_util.h
	$(CC) $(CPPFLAGS) -I. $(CFLAGS) -c -o $@ $<

clean:
	-rm -f *.o
	-rm -f aws
