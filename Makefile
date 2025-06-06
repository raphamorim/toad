CC = gcc
CFLAGS = -Wall -Wextra -std=c99
LIBS = -lncurses -lutil
TARGET = toad
SRCDIR = src
VTEDIR = $(SRCDIR)/vte
TESTDIR = tests

# Source files
MAIN_SOURCES = $(SRCDIR)/main.c
VTE_SOURCES = $(VTEDIR)/vte_parser.c $(VTEDIR)/vte_terminal.c
SOURCES = $(MAIN_SOURCES) $(VTE_SOURCES)

# Object files
MAIN_OBJECTS = $(MAIN_SOURCES:.c=.o)
VTE_OBJECTS = $(VTE_SOURCES:.c=.o)
OBJECTS = $(MAIN_OBJECTS) $(VTE_OBJECTS)

# Test files
TEST_SOURCES = $(TESTDIR)/test_vte.c
TEST_TARGET = $(TESTDIR)/test_vte
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)

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

# Compile test source
$(TESTDIR)/%.o: $(TESTDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c $< -o $@

# Build and run tests
test: $(TEST_TARGET)
	@echo "Running VTE parser tests..."
	@./$(TEST_TARGET)

# Build test executable
$(TEST_TARGET): $(TEST_OBJECTS) $(VTE_OBJECTS)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $(TEST_TARGET) $(TEST_OBJECTS) $(VTE_OBJECTS)

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
	rm -f $(TARGET) $(OBJECTS) $(TEST_TARGET) $(TEST_OBJECTS)

# Install dependencies (macOS)
install-deps:
	@echo "Installing ncurses..."
	@if command -v brew >/dev/null 2>&1; then \
		brew install ncurses; \
	else \
		echo "Please install ncurses manually"; \
	fi

.PHONY: all debug release run clean install-deps test