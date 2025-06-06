CC = gcc
CFLAGS = -Wall -Wextra -std=c99
LIBS = -lncurses -lutil
TARGET = toad
SRCDIR = src
VTEDIR = $(SRCDIR)/vte

# Source files
MAIN_SOURCES = $(SRCDIR)/main.c
VTE_SOURCES = $(VTEDIR)/vte_parser.c
SOURCES = $(MAIN_SOURCES) $(VTE_SOURCES)

# Object files
MAIN_OBJECTS = $(MAIN_SOURCES:.c=.o)
VTE_OBJECTS = $(VTE_SOURCES:.c=.o)
OBJECTS = $(MAIN_OBJECTS) $(VTE_OBJECTS)

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS) $(LIBS)

# Compile main source
$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile VTE source
$(VTEDIR)/%.o: $(VTEDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -f $(TARGET) $(OBJECTS)

# Install dependencies (macOS)
install-deps:
	@echo "Installing ncurses..."
	@if command -v brew >/dev/null 2>&1; then \
		brew install ncurses; \
	else \
		echo "Please install ncurses manually"; \
	fi

.PHONY: all debug release run clean install-deps