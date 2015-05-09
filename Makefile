all: cproxy sproxy

cproxy: cproxy.c
	gcc -Wall -g -o cproxy cproxy.c

sproxy: sproxy.c
	gcc -Wall -g -o sproxy sproxy.c

clean:
	rm -f *.o core cproxy sproxy


