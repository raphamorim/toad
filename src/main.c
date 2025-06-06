#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <util.h>

#include <ncurses.h>
#include "vte/vte_parser.h"

#define MAX_PANELS 4
#define BUFFER_SIZE 1024

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

void init_panel_screen(terminal_panel_t *panel) {
    panel->screen_width = panel->width - 2; // Account for borders
    panel->screen_height = panel->height - 2;
    
    // Allocate screen buffer
    panel->screen = malloc(panel->screen_height * sizeof(terminal_cell_t*));
    if (!panel->screen) {
        fprintf(stderr, "Failed to allocate screen buffer\n");
        exit(1);
    }
    
    for (int y = 0; y < panel->screen_height; y++) {
        panel->screen[y] = malloc(panel->screen_width * sizeof(terminal_cell_t));
        if (!panel->screen[y]) {
            fprintf(stderr, "Failed to allocate screen line %d\n", y);
            exit(1);
        }
        
        // Initialize cells
        for (int x = 0; x < panel->screen_width; x++) {
            panel->screen[y][x].codepoint = ' ';
            panel->screen[y][x].fg_color = -1;
            panel->screen[y][x].bg_color = -1;
            panel->screen[y][x].attrs = A_NORMAL;
        }
    }
    
    panel->cursor_x = 0;
    panel->cursor_y = 0;
    
    // Initialize VTE parser and set up terminal perform implementation
    vte_parser_init(&panel->parser);
    panel->perform = terminal_perform;
    panel->fg_color = -1;
    panel->bg_color = -1;
    panel->attrs = A_NORMAL;
}

void free_panel_screen(terminal_panel_t *panel) {
    if (panel->screen) {
        for (int y = 0; y < panel->screen_height; y++) {
            if (panel->screen[y]) {
                free(panel->screen[y]);
            }
        }
        free(panel->screen);
        panel->screen = NULL;
    }
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
    
    // Create ncurses window
    panel->win = newwin(height, width, y, x);
    if (!panel->win) {
        fprintf(stderr, "Failed to create window\n");
        return -1;
    }
    
    // Initialize screen buffer
    init_panel_screen(panel);
    
    // Create pseudo-terminal
    int slave_fd;
    if (openpty(&panel->master_fd, &slave_fd, NULL, NULL, NULL) == -1) {
        fprintf(stderr, "Failed to create pty: %s\n", strerror(errno));
        delwin(panel->win);
        free_panel_screen(panel);
        return -1;
    }
    
    // Fork child process
    panel->child_pid = fork();
    if (panel->child_pid == -1) {
        fprintf(stderr, "Failed to fork: %s\n", strerror(errno));
        close(panel->master_fd);
        close(slave_fd);
        delwin(panel->win);
        free_panel_screen(panel);
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
        
        // Set terminal size
        struct winsize ws;
        ws.ws_row = panel->screen_height;
        ws.ws_col = panel->screen_width;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);
        
        // Execute shell
        execl("/bin/zsh", "zsh", NULL);
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
    if (!panel || !panel->active || !panel->win || !panel->screen) {
        return;
    }
    
    werase(panel->win);
    box(panel->win, 0, 0);
    
    // Draw title
    if (panel == &mux.panels[mux.active_panel]) {
        wattron(panel->win, A_BOLD);
        mvwprintw(panel->win, 0, 2, " Terminal %d [ACTIVE] ", 
                  (int)(panel - mux.panels));
        wattroff(panel->win, A_BOLD);
    } else {
        mvwprintw(panel->win, 0, 2, " Terminal %d ", 
                  (int)(panel - mux.panels));
    }
    
    // Draw screen content
    for (int y = 0; y < panel->screen_height; y++) {
        for (int x = 0; x < panel->screen_width; x++) {
            terminal_cell_t *cell = &panel->screen[y][x];
            
            // Only draw non-space characters or characters with background colors
            if (cell->codepoint != ' ' || cell->bg_color != -1 || cell->attrs != A_NORMAL) {
                // Calculate color pair
                int color_pair = 0;
                if (cell->fg_color != -1 || cell->bg_color != -1) {
                    int fg = (cell->fg_color == -1) ? -1 : cell->fg_color;
                    int bg = (cell->bg_color == -1) ? -1 : cell->bg_color;
                    
                    // Find existing color pair or create new one
                    bool found = false;
                    for (int i = 1; i < COLOR_PAIRS && i < 64; i++) {
                        short pair_fg, pair_bg;
                        pair_content(i, &pair_fg, &pair_bg);
                        if (pair_fg == fg && pair_bg == bg) {
                            color_pair = i;
                            found = true;
                            break;
                        }
                    }
                    
                    // If not found, create new pair
                    if (!found) {
                        for (int i = 16; i < COLOR_PAIRS && i < 64; i++) { // Start from 16 to avoid conflicts
                            short pair_fg, pair_bg;
                            pair_content(i, &pair_fg, &pair_bg);
                            if (pair_fg == 0 && pair_bg == 0) { // Uninitialized pair
                                init_pair(i, fg, bg);
                                color_pair = i;
                                break;
                            }
                        }
                    }
                }
                
                // Apply attributes and colors
                if (cell->attrs != A_NORMAL) {
                    wattron(panel->win, cell->attrs);
                }
                if (color_pair > 0) {
                    wattron(panel->win, COLOR_PAIR(color_pair));
                }
                
                // Handle Unicode codepoints
                if (cell->codepoint <= 0x7F) {
                    // ASCII character
                    mvwaddch(panel->win, y + 1, x + 1, (chtype)cell->codepoint);
                } else {
                    // Unicode character - convert to UTF-8 and print
                    char utf8_buf[5] = {0};
                    if (cell->codepoint <= 0x7FF) {
                        utf8_buf[0] = 0xC0 | (cell->codepoint >> 6);
                        utf8_buf[1] = 0x80 | (cell->codepoint & 0x3F);
                    } else if (cell->codepoint <= 0xFFFF) {
                        utf8_buf[0] = 0xE0 | (cell->codepoint >> 12);
                        utf8_buf[1] = 0x80 | ((cell->codepoint >> 6) & 0x3F);
                        utf8_buf[2] = 0x80 | (cell->codepoint & 0x3F);
                    } else if (cell->codepoint <= 0x10FFFF) {
                        utf8_buf[0] = 0xF0 | (cell->codepoint >> 18);
                        utf8_buf[1] = 0x80 | ((cell->codepoint >> 12) & 0x3F);
                        utf8_buf[2] = 0x80 | ((cell->codepoint >> 6) & 0x3F);
                        utf8_buf[3] = 0x80 | (cell->codepoint & 0x3F);
                    } else {
                        // Invalid codepoint, use replacement character
                        utf8_buf[0] = '?';
                    }
                    mvwaddstr(panel->win, y + 1, x + 1, utf8_buf);
                }
                
                // Remove attributes and colors
                if (color_pair > 0) {
                    wattroff(panel->win, COLOR_PAIR(color_pair));
                }
                if (cell->attrs != A_NORMAL) {
                    wattroff(panel->win, cell->attrs);
                }
            } else {
                // For spaces with default colors, just put a space
                mvwaddch(panel->win, y + 1, x + 1, ' ');
            }
        }
    }
    
    // Highlight active panel border
    if (panel == &mux.panels[mux.active_panel]) {
        wattron(panel->win, A_BOLD);
        box(panel->win, 0, 0);
        wattroff(panel->win, A_BOLD);
    }
    
    wrefresh(panel->win);
}

void read_panel_data(terminal_panel_t *panel) {
    if (!panel || panel->master_fd < 0) {
        return;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(panel->master_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read > 0) {
        // Feed data to VTE parser
        vte_parser_feed(panel, buffer, bytes_read);
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
            
        default:
            // Send ALL other input directly to the active terminal
            if (active->master_fd >= 0) {
                if (ch == '\n' || ch == '\r') {
                    write(active->master_fd, "\r", 1);
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                    write(active->master_fd, "\b", 1);
                } else if (ch == KEY_LEFT) {
                    write(active->master_fd, "\033[D", 3);
                } else if (ch == KEY_RIGHT) {
                    write(active->master_fd, "\033[C", 3);
                } else if (ch == KEY_UP) {
                    write(active->master_fd, "\033[A", 3);
                } else if (ch == KEY_DOWN) {
                    write(active->master_fd, "\033[B", 3);
                } else if (ch >= 32 && ch <= 126) {
                    char c = ch;
                    write(active->master_fd, &c, 1);
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
    
    // Enable colors
    if (has_colors()) {
        start_color();
        use_default_colors();
        
        // Initialize basic color pairs (starting from 8 to avoid conflicts)
        init_pair(8, COLOR_RED, -1);
        init_pair(9, COLOR_GREEN, -1);
        init_pair(10, COLOR_YELLOW, -1);
        init_pair(11, COLOR_BLUE, -1);
        init_pair(12, COLOR_MAGENTA, -1);
        init_pair(13, COLOR_CYAN, -1);
        init_pair(14, COLOR_WHITE, -1);
        init_pair(15, COLOR_BLACK, -1);
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
        
        free_panel_screen(panel);
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
                "Active: %d | Tab: switch | q/ESC/Ctrl+C: quit", 
                mux.active_panel + 1);
        clrtoeol();
        refresh();
    }
    
    cleanup_multiplexer();
    return 0;
}