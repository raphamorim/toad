#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <util.h>

#include <ncurses.h>

#define MAX_PANELS 4
#define BUFFER_SIZE 1024
#define MAX_LINES 1000

typedef struct {
    WINDOW *win;
    int master_fd;
    pid_t child_pid;
    char **lines;
    int line_count;
    int scroll_offset;
    int active;
    int width, height;
    int start_x, start_y;
    int cursor_x, cursor_y;
    char current_line[BUFFER_SIZE];
    int current_line_pos;
} terminal_panel_t;

typedef struct {
    terminal_panel_t panels[MAX_PANELS];
    int panel_count;
    int active_panel;
    int screen_width, screen_height;
    int should_quit;
} multiplexer_t;

static multiplexer_t mux;

// Function declarations
void cleanup_multiplexer(void);

void cleanup_and_exit(int sig) {
    (void)sig;
    cleanup_multiplexer();
    exit(0);
}

void init_panel_lines(terminal_panel_t *panel) {
    panel->lines = calloc(MAX_LINES, sizeof(char*));
    if (!panel->lines) {
        fprintf(stderr, "Failed to allocate memory for panel lines\n");
        exit(1);
    }
    
    for (int i = 0; i < MAX_LINES; i++) {
        panel->lines[i] = calloc(BUFFER_SIZE, sizeof(char));
        if (!panel->lines[i]) {
            fprintf(stderr, "Failed to allocate memory for line %d\n", i);
            exit(1);
        }
    }
    
    panel->line_count = 0;
    panel->scroll_offset = 0;
}

void free_panel_lines(terminal_panel_t *panel) {
    if (panel->lines) {
        for (int i = 0; i < MAX_LINES; i++) {
            if (panel->lines[i]) {
                free(panel->lines[i]);
            }
        }
        free(panel->lines);
        panel->lines = NULL;
    }
}

void add_line_to_panel(terminal_panel_t *panel, const char *line) {
    if (!panel || !panel->lines || !line) {
        return;
    }
    
    if (panel->line_count >= MAX_LINES) {
        // Shift lines up
        if (panel->lines[0]) {
            free(panel->lines[0]);
        }
        for (int i = 0; i < MAX_LINES - 1; i++) {
            panel->lines[i] = panel->lines[i + 1];
        }
        panel->lines[MAX_LINES - 1] = calloc(BUFFER_SIZE, sizeof(char));
        if (!panel->lines[MAX_LINES - 1]) {
            return;
        }
        panel->line_count = MAX_LINES - 1;
    }
    
    strncpy(panel->lines[panel->line_count], line, BUFFER_SIZE - 1);
    panel->lines[panel->line_count][BUFFER_SIZE - 1] = '\0';
    panel->line_count++;
}

int create_terminal_panel(terminal_panel_t *panel, int x, int y, int width, int height) {
    if (!panel) {
        return -1;
    }
    
    // Initialize panel structure
    memset(panel, 0, sizeof(terminal_panel_t));
    panel->start_x = x;
    panel->start_y = y;
    panel->width = width;
    panel->height = height;
    panel->active = 1;
    panel->master_fd = -1;
    panel->child_pid = -1;
    panel->cursor_x = 1;
    panel->cursor_y = 1;
    panel->current_line_pos = 0;
    panel->current_line[0] = '\0';
    
    // Create ncurses window
    panel->win = newwin(height, width, y, x);
    if (!panel->win) {
        fprintf(stderr, "Failed to create window\n");
        return -1;
    }
    
    // Initialize line storage
    init_panel_lines(panel);
    
    // Create pseudo-terminal
    int slave_fd;
    if (openpty(&panel->master_fd, &slave_fd, NULL, NULL, NULL) == -1) {
        fprintf(stderr, "Failed to create pty: %s\n", strerror(errno));
        delwin(panel->win);
        free_panel_lines(panel);
        return -1;
    }
    
    // Fork child process
    panel->child_pid = fork();
    if (panel->child_pid == -1) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        close(panel->master_fd);
        close(slave_fd);
        delwin(panel->win);
        free_panel_lines(panel);
        return -1;
    }
    
    if (panel->child_pid == 0) {
        // Child process - exec shell
        setsid();
        
        // Redirect stdio to slave
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        
        close(panel->master_fd);
        close(slave_fd);
        
        // Execute shell
        execl("/bin/bash", "bash", NULL);
        exit(1);
    }
    
    // Parent process - close slave fd and set non-blocking
    close(slave_fd);
    int flags = fcntl(panel->master_fd, F_GETFL);
    fcntl(panel->master_fd, F_SETFL, flags | O_NONBLOCK);
    
    // Initial draw
    box(panel->win, 0, 0);
    wrefresh(panel->win);
    
    return 0;
}

void draw_panel(terminal_panel_t *panel) {
    if (!panel || !panel->active || !panel->win) {
        return;
    }
    
    werase(panel->win);
    box(panel->win, 0, 0);
    
    // Draw title
    mvwprintw(panel->win, 0, 2, " Terminal %d ", 
              (int)(panel - mux.panels));
    
    // Draw content
    int display_height = panel->height - 3; // Account for borders and current line
    int start_line = panel->scroll_offset;
    int end_line = start_line + display_height;
    
    if (end_line > panel->line_count) {
        end_line = panel->line_count;
    }
    
    for (int i = start_line; i < end_line; i++) {
        if (i >= 0 && i < panel->line_count && panel->lines[i]) {
            int display_width = panel->width - 2; // Account for borders
            char display_line[display_width + 1];
            strncpy(display_line, panel->lines[i], display_width);
            display_line[display_width] = '\0';
            mvwprintw(panel->win, i - start_line + 1, 1, "%s", display_line);
        }
    }
    
    // Draw current input line
    int input_line_y = panel->height - 2;
    mvwprintw(panel->win, input_line_y, 1, "$ %s", panel->current_line);
    
    // Show cursor in active panel
    if (panel == &mux.panels[mux.active_panel]) {
        wattron(panel->win, A_BOLD);
        box(panel->win, 0, 0);
        wattroff(panel->win, A_BOLD);
        
        // Position cursor
        int cursor_pos = 3 + panel->current_line_pos; // 3 = "$ " + current pos
        if (cursor_pos < panel->width - 1) {
            wmove(panel->win, input_line_y, cursor_pos);
        }
    }
    
    wrefresh(panel->win);
}

void read_panel_data(terminal_panel_t *panel) {
    if (!panel || panel->master_fd < 0) {
        return;
    }
    
    char buffer[BUFFER_SIZE];
    static char line_buffers[MAX_PANELS][BUFFER_SIZE] = {{0}};
    static int line_positions[MAX_PANELS] = {0};
    
    int panel_idx = panel - mux.panels;
    if (panel_idx < 0 || panel_idx >= MAX_PANELS) {
        return;
    }
    
    ssize_t bytes_read = read(panel->master_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        
        // Process character by character
        for (int i = 0; i < bytes_read; i++) {
            char c = buffer[i];
            
            if (c == '\n' || c == '\r') {
                if (line_positions[panel_idx] > 0) {
                    line_buffers[panel_idx][line_positions[panel_idx]] = '\0';
                    add_line_to_panel(panel, line_buffers[panel_idx]);
                    line_positions[panel_idx] = 0;
                }
            } else if (c >= 32 && c <= 126) { // Printable characters
                if (line_positions[panel_idx] < BUFFER_SIZE - 1) {
                    line_buffers[panel_idx][line_positions[panel_idx]++] = c;
                }
            }
        }
        
        // Auto-scroll to bottom
        if (panel->line_count > panel->height - 3) {
            panel->scroll_offset = panel->line_count - (panel->height - 3);
        }
    } else if (bytes_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Error reading from pty
        panel->active = 0;
    }
}

void handle_input() {
    int ch = getch();
    if (ch == ERR) {
        return; // No input available
    }
    
    // IMMEDIATE quit check - handle before anything else
    if (ch == 113 || ch == 81 || ch == 27 || ch == 3) { // q, Q, ESC, Ctrl+C
        mux.should_quit = 1;
        cleanup_multiplexer(); // Force immediate cleanup
        exit(0); // Force exit
    }
    
    if (mux.active_panel >= mux.panel_count) {
        return;
    }
    
    terminal_panel_t *active = &mux.panels[mux.active_panel];
    
    switch (ch) {
        case '\t': // Switch panels
            mux.active_panel = (mux.active_panel + 1) % mux.panel_count;
            break;
            
        case KEY_UP:
            if (active->scroll_offset > 0) {
                active->scroll_offset--;
            }
            break;
            
        case KEY_DOWN:
            if (active->scroll_offset < active->line_count - (active->height - 3)) {
                active->scroll_offset++;
            }
            break;
            
        case KEY_LEFT:
            if (active->current_line_pos > 0) {
                active->current_line_pos--;
            }
            break;
            
        case KEY_RIGHT:
            if (active->current_line_pos < (int)strlen(active->current_line)) {
                active->current_line_pos++;
            }
            break;
            
        case KEY_BACKSPACE:
        case 127: // DEL key
        case 8:   // Backspace
            if (active->current_line_pos > 0) {
                // Remove character from current line
                memmove(&active->current_line[active->current_line_pos - 1],
                       &active->current_line[active->current_line_pos],
                       strlen(active->current_line) - active->current_line_pos + 1);
                active->current_line_pos--;
                
                // Send backspace to terminal
                if (active->master_fd >= 0) {
                    write(active->master_fd, "\b", 1);
                }
            }
            break;
            
        case '\n':
        case '\r':
            // Send the complete line to terminal
            if (active->master_fd >= 0) {
                write(active->master_fd, active->current_line, strlen(active->current_line));
                write(active->master_fd, "\n", 1);
            }
            
            // Clear current line
            active->current_line[0] = '\0';
            active->current_line_pos = 0;
            break;
            
        default:
            // Add printable characters to current line
            if (ch >= 32 && ch <= 126) {
                int len = strlen(active->current_line);
                if (len < BUFFER_SIZE - 1) {
                    // Insert character at cursor position
                    memmove(&active->current_line[active->current_line_pos + 1],
                           &active->current_line[active->current_line_pos],
                           len - active->current_line_pos + 1);
                    active->current_line[active->current_line_pos] = ch;
                    active->current_line_pos++;
                    
                    // Send character to terminal
                    if (active->master_fd >= 0) {
                        char c = ch;
                        write(active->master_fd, &c, 1);
                    }
                }
            }
            break;
    }
}

void init_multiplexer() {
    // Initialize multiplexer structure
    memset(&mux, 0, sizeof(multiplexer_t));
    mux.should_quit = 0;
    
    // Initialize ncurses
    initscr();
    if (stdscr == NULL) {
        fprintf(stderr, "Failed to initialize ncurses\n");
        exit(1);
    }
    
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    
    getmaxyx(stdscr, mux.screen_height, mux.screen_width);
    
    if (mux.screen_width < 20 || mux.screen_height < 10) {
        endwin();
        fprintf(stderr, "Terminal too small\n");
        exit(1);
    }
    
    // Create initial panels
    mux.panel_count = 2;
    mux.active_panel = 0;
    
    // Split screen into 2 panels
    int panel_width = mux.screen_width / 2;
    int panel_height = mux.screen_height - 1; // Leave space for status line
    
    if (create_terminal_panel(&mux.panels[0], 0, 0, 
                             panel_width, panel_height) == -1) {
        endwin();
        fprintf(stderr, "Failed to create panel 0\n");
        exit(1);
    }
    
    if (create_terminal_panel(&mux.panels[1], panel_width, 0, 
                             mux.screen_width - panel_width, panel_height) == -1) {
        endwin();
        fprintf(stderr, "Failed to create panel 1\n");
        exit(1);
    }
    
    // Initial draw
    clear();
    refresh();
}

void cleanup_multiplexer() {
    // Kill child processes first
    for (int i = 0; i < mux.panel_count; i++) {
        terminal_panel_t *panel = &mux.panels[i];
        
        if (panel->child_pid > 0) {
            kill(panel->child_pid, SIGKILL); // Use SIGKILL for immediate termination
            waitpid(panel->child_pid, NULL, WNOHANG); // Non-blocking wait
        }
        
        if (panel->master_fd >= 0) {
            close(panel->master_fd);
        }
        
        if (panel->win) {
            delwin(panel->win);
        }
        
        free_panel_lines(panel);
    }
    
    // Restore terminal state
    if (stdscr) {
        clear();
        refresh();
        endwin();
    }
    
    // Reset terminal
    printf("\033[?1049l"); // Exit alternate screen
    printf("\033[0m");     // Reset colors
    printf("\033[2J");     // Clear screen
    printf("\033[H");      // Move cursor to home
    fflush(stdout);
}

int main() {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);
    
    init_multiplexer();
    
    fd_set read_fds;
    struct timeval timeout;
    
    while (!mux.should_quit) {
        // Prepare file descriptor set
        FD_ZERO(&read_fds);
        int max_fd = 0;
        
        for (int i = 0; i < mux.panel_count; i++) {
            if (mux.panels[i].active && mux.panels[i].master_fd >= 0) {
                FD_SET(mux.panels[i].master_fd, &read_fds);
                if (mux.panels[i].master_fd > max_fd) {
                    max_fd = mux.panels[i].master_fd;
                }
            }
        }
        
        // Set timeout for select
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000; // 50ms
        
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready > 0) {
            // Read data from terminals
            for (int i = 0; i < mux.panel_count; i++) {
                if (mux.panels[i].active && mux.panels[i].master_fd >= 0 &&
                    FD_ISSET(mux.panels[i].master_fd, &read_fds)) {
                    read_panel_data(&mux.panels[i]);
                }
            }
        }
        
        // Handle user input
        handle_input();
        
        // Check quit condition again
        if (mux.should_quit) {
            break;
        }
        
        // Redraw panels
        for (int i = 0; i < mux.panel_count; i++) {
            if (mux.panels[i].active) {
                draw_panel(&mux.panels[i]);
            }
        }
        
        // Status line
        mvprintw(mux.screen_height - 1, 0, 
                "Active: %d | Tab: switch | q/ESC/Ctrl+C: quit | ↑↓: scroll | ←→: cursor", 
                mux.active_panel + 1);
        clrtoeol();
        refresh();
    }
    
    cleanup_multiplexer();
    return 0;
}