CC = gcc
CFLAGS = -Wall -Wextra -std=c99
LIBS = -lncurses -lutil
TARGET = toad
SRCDIR = src
SOURCES = $(SRCDIR)/main.c

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES) $(LIBS)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: $(TARGET)

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: $(TARGET)

# Run the program
run: $(TARGET)
	./$(TARGET)

# Clean build artifacts
clean:
	rm -f $(TARGET)

# Install dependencies (macOS)
install-deps:
	@echo "Installing ncurses..."
	@if command -v brew >/dev/null 2>&1; then \
		brew install ncurses; \
	else \
		echo "Please install ncurses manually"; \
	fi

.PHONY: all debug release run clean install-deps