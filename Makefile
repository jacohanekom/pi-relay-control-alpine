CXX      = g++
CXXFLAGS = -Wall -O2 -std=c++17 -static-libgcc -static-libstdc++ -pthread
LDFLAGS  = -llgpio
TARGET   = relay_control
SRCDIR   = src

all: $(TARGET)

$(TARGET): $(SRCDIR)/main.cpp
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCDIR)/main.cpp $(LDFLAGS)

install: all
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc
	install -m 644 pi-relay-control.conf $(DESTDIR)/etc/pi-relay-control.conf
	install -d $(DESTDIR)/etc/init.d
	install -m 755 openrc/pi-relay-control.initd $(DESTDIR)/etc/init.d/pi-relay-control
	install -d $(DESTDIR)/var/lib/relay_control

clean:
	rm -f $(TARGET)
