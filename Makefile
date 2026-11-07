CFLAGS= -D__LINUX__ -c -Wall -O2
LIBS = -lm -lpthread -lgpiod

all: bb_ir

bb_ir: main.o
	gcc main.o $(LIBS) -o bb_ir

main.o: main.cpp
	g++ $(CFLAGS) main.cpp

clean:
	rm *.o bb_ir 
