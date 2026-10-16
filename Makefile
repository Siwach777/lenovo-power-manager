CXX ?= g++
CXXFLAGS ?= -std=c++20 -O3 -march=native -flto -DNDEBUG -Wall -Wextra -Iinclude $(shell pkg-config --cflags libsystemd)
LDFLAGS ?= -flto $(shell pkg-config --libs libsystemd)
PREFIX ?= /usr/local
USER_PREFIX ?= $(HOME)/.local

SRCS = src/main.cpp src/hardware.cpp src/notifier.cpp src/service.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = legion-power

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)
	strip $(TARGET)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

install-user: $(TARGET)
	install -d $(USER_PREFIX)/bin
	install -m 755 $(TARGET) $(USER_PREFIX)/bin/
	ln -sf $(USER_PREFIX)/bin/$(TARGET) $(USER_PREFIX)/bin/power-manager

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	ln -sf $(DESTDIR)$(PREFIX)/bin/$(TARGET) $(DESTDIR)$(PREFIX)/bin/power-manager

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean install install-user
