CXX    = g++
CFLAGS = -Wall -Wextra -std=gnu++11 -O3 -march=native
LIBS   = -pthread -ljansson -lcurl -lgmp -lgmpxx -lcrypto

all: rieMiner

rieMiner: main.o miner.o stratumclient.o gbtclient.o client.o tools.o
	$(CXX) $(CFLAGS) -o rieMiner $^ $(LIBS)

main.o: main.cpp
	$(CXX) $(CFLAGS) -c -o main.o main.cpp $(LIBS)

miner.o: miner.cpp
	$(CXX) $(CFLAGS) -c -o miner.o miner.cpp $(LIBS)

stratumclient.o: stratumclient.cpp
	$(CXX) $(CFLAGS) -c -o stratumclient.o stratumclient.cpp $(LIBS)

gbtclient.o: gbtclient.cpp
	$(CXX) $(CFLAGS) -c -o gbtclient.o gbtclient.cpp $(LIBS)

client.o: client.cpp
	$(CXX) $(CFLAGS) -c -o client.o client.cpp $(LIBS)

tools.o: tools.cpp
	$(CXX) $(CFLAGS) -c -o tools.o tools.cpp $(LIBS)

clean:
	rm -rf rieMiner *.o