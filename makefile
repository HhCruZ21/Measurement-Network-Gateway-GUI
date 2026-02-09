# ============================================================================
# GTK Application Makefile
# Project: GUI Application
# File: gui.c and Makefile are in the same directory
# ============================================================================

# Compiler settings
CC = gcc
CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -Wall -Wextra -Wpedantic -O2 -g
LDFLAGS = $(shell pkg-config --libs gtk+-3.0)

# Target executable
TARGET = gui_app

# Source files (only gui.c in current directory)
SRC = gui.c utils.c
OBJ = $(SRC:.c=.o)

# Default target - build the application
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJ)
	@echo "🔗 Linking $(TARGET)..."
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)
	@echo "✅ Build successful! Executable: ./$(TARGET)"
	@echo "📏 File size: $(shell ls -lh $(TARGET) | awk '{print $$5}')"

# Compile source files
%.o: %.c
	@echo "⚙️  Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Run the application
run: $(TARGET)
	@echo "🚀 Running $(TARGET)..."
	@echo "========================================"
	./$(TARGET)

# Debug build with extra flags
debug: CFLAGS = $(shell pkg-config --cflags gtk+-3.0) -Wall -Wextra -Wpedantic -O0 -g -DDEBUG -fsanitize=address
debug: clean $(TARGET)
	@echo "🐛 Debug build complete with address sanitizer"

# Clean build artifacts
clean:
	@echo "🧹 Cleaning up..."
	rm -f $(OBJ) $(TARGET) *.d *~ *.bak
	@echo "✅ Clean complete"

# Install required dependencies
deps:
	@echo "📦 Installing dependencies..."
	sudo apt-get update
	sudo apt-get install -y \
		libgtk-3-dev \
		pkg-config \
		build-essential \
		gdb \
		valgrind
	@echo "✅ Dependencies installed"

# Check GTK configuration
check:
	@echo "🔍 Checking GTK configuration..."
	@echo "GTK Version: $(shell pkg-config --modversion gtk+-3.0)"
	@echo ""
	@echo "Include paths:"
	@pkg-config --cflags gtk+-3.0 | tr ' ' '\n' | grep '^-I' | sed 's/^-I//'
	@echo ""
	@echo "Library flags:"
	@pkg-config --libs gtk+-3.0

# Create a backup of the source file
backup:
	@echo "💾 Creating backup..."
	cp gui.c gui.c.backup_$(shell date +%Y%m%d_%H%M%S)
	@echo "✅ Backup created"

# Show file information
info:
	@echo "📁 Project Information"
	@echo "======================"
	@echo "Directory: $(shell pwd)"
	@echo "Source file: $(SRC)"
	@echo "Object file: $(OBJ)"
	@echo "Target: $(TARGET)"
	@echo "GTK flags: $(CFLAGS)"
	@echo ""
	@echo "📊 Source file stats:"
	@wc -l $(SRC)
	@echo ""
	@echo "🗂️  Directory contents:"
	@ls -la

# Build and run in one command
br: clean $(TARGET) run

# Test compilation without linking
compile-test:
	@echo "🧪 Testing compilation..."
	$(CC) $(CFLAGS) -c $(SRC) -o test.o
	@echo "✅ Compilation test passed"
	rm -f test.o

# Run with GDB debugger
gdb: debug
	@echo "🐞 Starting GDB debugger..."
	gdb ./$(TARGET)

# Run with Valgrind for memory checking
valgrind: $(TARGET)
	@echo "🧠 Running Valgrind memory check..."
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Create a simple test GTK app if gui.c doesn't exist
init:
	@if [ ! -f gui.c ]; then \
		echo "📝 Creating initial gui.c..."; \
		echo '#include <gtk/gtk.h>\n\nstatic void button_clicked(GtkWidget *widget, gpointer data) {\n    g_print("Button clicked!\\n");\n}\n\nint main(int argc, char *argv[]) {\n    GtkWidget *window;\n    GtkWidget *button;\n    GtkWidget *vbox;\n    \n    gtk_init(&argc, &argv);\n    \n    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);\n    gtk_window_set_title(GTK_WINDOW(window), "My GTK App");\n    gtk_window_set_default_size(GTK_WINDOW(window), 400, 300);\n    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);\n    \n    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);\n    gtk_container_add(GTK_CONTAINER(window), vbox);\n    \n    button = gtk_button_new_with_label("Click Me!");\n    g_signal_connect(button, "clicked", G_CALLBACK(button_clicked), NULL);\n    gtk_box_pack_start(GTK_BOX(vbox), button, FALSE, FALSE, 0);\n    \n    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);\n    \n    gtk_widget_show_all(window);\n    gtk_main();\n    \n    return 0;\n}' > gui.c; \
		echo "✅ gui.c created successfully"; \
	else \
		echo "📄 gui.c already exists"; \
	fi

# Help command
help:
	@echo "📖 Makefile Help"
	@echo "================"
	@echo "make           - Build the application"
	@echo "make run       - Build and run the application"
	@echo "make br        - Clean, build, and run (quick)"
	@echo "make debug     - Build with debug symbols"
	@echo "make clean     - Remove build files"
	@echo "make check     - Show GTK configuration"
	@echo "make deps      - Install dependencies"
	@echo "make gdb       - Run with GDB debugger"
	@echo "make valgrind  - Run with Valgrind memory check"
	@echo "make backup    - Backup source file"
	@echo "make info      - Show project information"
	@echo "make init      - Create initial gui.c if missing"
	@echo "make compile-test - Test compilation only"
	@echo "make help      - Show this help"

.PHONY: all run debug clean deps check backup info br gdb valgrind init compile-test help