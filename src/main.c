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
#include <locale.h>
#include <time.h>

#include <ncurses.h>
#include "vte/vte_parser.h"

#define MAX_PANELS 8
#define BUFFER_SIZE 1024
#define CTRL_KEY(k) ((k) & 0x1f)

typedef enum {
    MODE_NORMAL,    // All input goes to terminal
    MODE_COMMAND    // Waiting for command key
} input_mode_t;

typedef enum {
    PANEL_TYPE_MAIN,    // Full-screen main panel
    PANEL_TYPE_OVERLAY  // Smaller overlay panel
} panel_type_t;

typedef struct {
    terminal_panel_t panels[MAX_PANELS];
    panel_type_t panel_types[MAX_PANELS];
    int panel_z_order[MAX_PANELS];  // Z-order for rendering (higher index = front)
    bool panel_dirty[MAX_PANELS];   // Track which panels need redrawing
    int panel_count;
    int active_panel;
    int screen_width, screen_height;
    int should_quit;
    
    // Input mode system
    input_mode_t mode;
    int ctrl_count;
    
    // Rendering optimization
    bool force_full_redraw;
    bool status_line_dirty;
} multiplexer_t;

static multiplexer_t mux;

// Function declarations
void cleanup_multiplexer(void);
void enter_command_mode(void);
void exit_command_mode(void);
int create_overlay_panel(void);
void bring_panel_to_front(int panel_index);
void close_panel(int panel_index);
void mark_panel_dirty(int panel_index);
void mark_all_panels_dirty(void);
void mark_status_dirty(void);
void draw_background_pattern(void);
void draw_colorful_border(WINDOW *win, bool active, panel_type_t type);

void cleanup_and_exit(int sig) {
    (void)sig;
    cleanup_multiplexer();
    exit(0);
}

// Helper functions for mode system
void enter_command_mode(void) {
    mux.mode = MODE_COMMAND;
    mux.ctrl_count = 0;
    mark_status_dirty();
}

void exit_command_mode(void) {
    mux.mode = MODE_NORMAL;
    mux.ctrl_count = 0;
    mark_status_dirty();
}

void mark_panel_dirty(int panel_index) {
    if (panel_index >= 0 && panel_index < mux.panel_count) {
        mux.panel_dirty[panel_index] = true;
    }
}

void mark_all_panels_dirty(void) {
    for (int i = 0; i < mux.panel_count; i++) {
        mux.panel_dirty[i] = true;
    }
    mux.force_full_redraw = true;
}

void mark_status_dirty(void) {
    mux.status_line_dirty = true;
}

void draw_background_pattern(void) {
    // Draw a decorative pattern background inspired by retro interfaces
    // Use different characters and colors to create a charming pattern
    
    for (int y = 0; y < mux.screen_height - 1; y++) { // Leave space for status line
        for (int x = 0; x < mux.screen_width; x++) {
            char pattern_char = ' ';
            int color_pair = 0;
            
            // Create a decorative pattern with dots, stars, and other characters
            int pattern_x = x % 8;
            int pattern_y = y % 6;
            
            if ((pattern_x == 1 && pattern_y == 1) || (pattern_x == 6 && pattern_y == 4)) {
                pattern_char = '*';
                color_pair = 9; // Green stars
            } else if ((pattern_x == 3 && pattern_y == 2) || (pattern_x == 5 && pattern_y == 5)) {
                pattern_char = '.';
                color_pair = 11; // Blue dots
            } else if ((pattern_x == 0 && pattern_y == 3) || (pattern_x == 7 && pattern_y == 0)) {
                pattern_char = '+';
                color_pair = 12; // Magenta plus signs
            } else if ((pattern_x == 2 && pattern_y == 4) || (pattern_x == 4 && pattern_y == 1)) {
                pattern_char = 'o';
                color_pair = 13; // Cyan circles
            } else {
                // Background with very subtle pattern
                if ((x + y) % 4 == 0) {
                    pattern_char = '.';
                    color_pair = 15; // Very dim
                } else {
                    pattern_char = ' ';
                    color_pair = 0;
                }
            }
            
            if (color_pair > 0) {
                attron(COLOR_PAIR(color_pair));
            }
            mvaddch(y, x, pattern_char);
            if (color_pair > 0) {
                attroff(COLOR_PAIR(color_pair));
            }
        }
    }
}

void draw_colorful_border(WINDOW *win, bool active, panel_type_t type) {
    // Draw colorful borders for panels
    int border_color = 0;
    
    if (active) {
        // Active panels get bright, colorful borders
        if (type == PANEL_TYPE_OVERLAY) {
            border_color = 12; // Bright magenta for active overlays
        } else {
            border_color = 10; // Bright yellow for active main panel
        }
        wattron(win, COLOR_PAIR(border_color) | A_BOLD);
    } else {
        // Inactive panels get softer colors
        if (type == PANEL_TYPE_OVERLAY) {
            border_color = 11; // Blue for inactive overlays
        } else {
            border_color = 9; // Green for inactive main panel
        }
        wattron(win, COLOR_PAIR(border_color));
    }
    
    // Draw the border
    box(win, 0, 0);
    
    // Add corner decorations for active panels
    if (active) {
        mvwaddch(win, 0, 0, '+');
        mvwaddch(win, 0, getmaxx(win) - 1, '+');
        mvwaddch(win, getmaxy(win) - 1, 0, '+');
        mvwaddch(win, getmaxy(win) - 1, getmaxx(win) - 1, '+');
    }
    
    if (border_color > 0) {
        wattroff(win, COLOR_PAIR(border_color));
        if (active) {
            wattroff(win, A_BOLD);
        }
    }
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
    
    // Initialize with enhanced terminal functions
    terminal_panel_init(panel, panel->screen_width, panel->screen_height);
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

int create_terminal_panel(terminal_panel_t *panel, int x, int y, int width, int height, panel_type_t type) {
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

void draw_panel(terminal_panel_t *panel, int panel_index) {
    if (!panel || !panel->active || !panel->win || !panel->screen) {
        return;
    }
    
    werase(panel->win);
    
    // Draw different styles based on panel type
    panel_type_t type = mux.panel_types[panel_index];
    bool is_active = (panel_index == mux.active_panel);
    
    // Use the colorful border function
    draw_colorful_border(panel->win, is_active, type);
    
    // Add shadow effect for overlay panels
    if (type == PANEL_TYPE_OVERLAY) {
        if (panel->start_x + panel->width < mux.screen_width && 
            panel->start_y + panel->height < mux.screen_height) {
            // Draw shadow with color
            attron(COLOR_PAIR(15)); // Dim color for shadow
            for (int y = 1; y <= panel->height; y++) {
                mvaddch(panel->start_y + y, panel->start_x + panel->width, ':');
            }
            for (int x = 1; x <= panel->width; x++) {
                mvaddch(panel->start_y + panel->height, panel->start_x + x, '.');
            }
            attroff(COLOR_PAIR(15));
        }
    }
    
    // Draw colorful title with panel type indicator
    int title_color = is_active ? (type == PANEL_TYPE_OVERLAY ? 12 : 10) : 
                                 (type == PANEL_TYPE_OVERLAY ? 11 : 9);
    
    wattron(panel->win, COLOR_PAIR(title_color));
    if (is_active) {
        wattron(panel->win, A_BOLD);
        if (type == PANEL_TYPE_OVERLAY) {
            mvwprintw(panel->win, 0, 2, " ✨ Overlay %d [ACTIVE] ✨ ", panel_index);
        } else {
            mvwprintw(panel->win, 0, 2, " 🖥️  Main Terminal [ACTIVE] 🖥️  ");
        }
        wattroff(panel->win, A_BOLD);
    } else {
        if (type == PANEL_TYPE_OVERLAY) {
            mvwprintw(panel->win, 0, 2, " ⭐ Overlay %d ⭐ ", panel_index);
        } else {
            mvwprintw(panel->win, 0, 2, " 💻 Main Terminal 💻 ");
        }
    }
    wattroff(panel->win, COLOR_PAIR(title_color));
    
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
    if (panel_index == mux.active_panel) {
        wattron(panel->win, A_BOLD);
        box(panel->win, 0, 0);
        wattroff(panel->win, A_BOLD);
    }
    
    // Position the real cursor for active panel
    if (panel_index == mux.active_panel && 
        panel->cursor_y >= 0 && panel->cursor_y < panel->screen_height &&
        panel->cursor_x >= 0 && panel->cursor_x < panel->screen_width) {
        wmove(panel->win, panel->cursor_y + 1, panel->cursor_x + 1);
    }
    
    // Use wnoutrefresh instead of wrefresh to reduce flickering
    // The actual refresh will happen once at the end of the main loop
    wnoutrefresh(panel->win);
}

int create_overlay_panel(void) {
    if (mux.panel_count >= MAX_PANELS) {
        return -1; // No more panels available
    }
    
    // Calculate overlay panel size and position (centered, 50% of screen size - smaller than main)
    int overlay_width = (mux.screen_width * 1) / 2;
    int overlay_height = (mux.screen_height * 1) / 2;
    int overlay_x = (mux.screen_width - overlay_width) / 2;
    int overlay_y = (mux.screen_height - overlay_height) / 2;
    
    // Ensure minimum size
    if (overlay_width < 25) overlay_width = 25;
    if (overlay_height < 12) overlay_height = 12;
    
    int panel_index = mux.panel_count;
    
    if (create_terminal_panel(&mux.panels[panel_index], overlay_x, overlay_y, 
                             overlay_width, overlay_height, PANEL_TYPE_OVERLAY) == -1) {
        return -1;
    }
    
    // Set panel type and add to z-order
    mux.panel_types[panel_index] = PANEL_TYPE_OVERLAY;
    mux.panel_z_order[panel_index] = panel_index; // Higher index = front
    mux.panel_count++;
    
    // Bring new panel to front and make it active
    bring_panel_to_front(panel_index);
    mux.active_panel = panel_index;
    
    return panel_index;
}

void bring_panel_to_front(int panel_index) {
    if (panel_index < 0 || panel_index >= mux.panel_count) {
        return;
    }
    
    // Find current z-order position
    int current_z = mux.panel_z_order[panel_index];
    
    // Move all panels with higher z-order down by one
    for (int i = 0; i < mux.panel_count; i++) {
        if (mux.panel_z_order[i] > current_z) {
            mux.panel_z_order[i]--;
        }
    }
    
    // Put this panel at the front
    mux.panel_z_order[panel_index] = mux.panel_count - 1;
}

void close_panel(int panel_index) {
    if (panel_index < 0 || panel_index >= mux.panel_count || panel_index == 0) {
        return; // Can't close main panel (index 0)
    }
    
    terminal_panel_t *panel = &mux.panels[panel_index];
    
    // Kill child process
    if (panel->child_pid > 0) {
        kill(panel->child_pid, SIGKILL);
        waitpid(panel->child_pid, NULL, WNOHANG);
    }
    
    // Close file descriptor
    if (panel->master_fd >= 0) {
        close(panel->master_fd);
    }
    
    // Free window and screen
    if (panel->win) {
        delwin(panel->win);
    }
    free_panel_screen(panel);
    
    // Mark as inactive
    panel->active = 0;
    
    // Adjust z-order for remaining panels
    int closed_z = mux.panel_z_order[panel_index];
    for (int i = 0; i < mux.panel_count; i++) {
        if (i != panel_index && mux.panel_z_order[i] > closed_z) {
            mux.panel_z_order[i]--;
        }
    }
    
    // Compact panel arrays by moving last panel to closed position
    if (panel_index < mux.panel_count - 1) {
        mux.panels[panel_index] = mux.panels[mux.panel_count - 1];
        mux.panel_types[panel_index] = mux.panel_types[mux.panel_count - 1];
        mux.panel_z_order[panel_index] = mux.panel_z_order[mux.panel_count - 1];
        
        // Update z-order references
        for (int i = 0; i < mux.panel_count; i++) {
            if (mux.panel_z_order[i] == mux.panel_z_order[panel_index]) {
                mux.panel_z_order[i] = closed_z;
                break;
            }
        }
    }
    
    mux.panel_count--;
    
    // Switch to main panel if we closed the active panel
    if (mux.active_panel == panel_index || mux.active_panel >= mux.panel_count) {
        mux.active_panel = 0; // Switch to main panel
    }
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
        
        // Mark this panel as dirty since it received new data
        int panel_index = panel - mux.panels;
        mark_panel_dirty(panel_index);
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
    
    if (mux.active_panel >= mux.panel_count) {
        return;
    }
    
    terminal_panel_t *active = &mux.panels[mux.active_panel];
    
    // Handle input based on current mode
    if (mux.mode == MODE_COMMAND) {
        // Command mode - handle multiplexer commands
        switch (ch) {
            case 'q':
            case 'Q':
                // Quit
                mux.should_quit = 1;
                cleanup_multiplexer();
                exit(0);
                break;
                
            case '\t':
            case 'n':
            case 'N':
                // Switch to next panel
                mark_panel_dirty(mux.active_panel); // Mark old panel dirty
                mux.active_panel = (mux.active_panel + 1) % mux.panel_count;
                mark_panel_dirty(mux.active_panel); // Mark new panel dirty
                exit_command_mode();
                break;
                
            case 'p':
            case 'P':
                // Switch to previous panel
                mark_panel_dirty(mux.active_panel); // Mark old panel dirty
                mux.active_panel = (mux.active_panel - 1 + mux.panel_count) % mux.panel_count;
                mark_panel_dirty(mux.active_panel); // Mark new panel dirty
                exit_command_mode();
                break;
                
            case 'c':
            case 'C':
                // Create new overlay panel
                create_overlay_panel();
                mark_all_panels_dirty(); // New panel affects rendering
                exit_command_mode();
                break;
                
            case 'x':
            case 'X':
                // Close current panel (if not main panel)
                if (mux.active_panel > 0) {
                    close_panel(mux.active_panel);
                    mark_all_panels_dirty(); // Panel removal affects rendering
                }
                exit_command_mode();
                break;
                
            case 'f':
            case 'F':
                // Bring current panel to front
                bring_panel_to_front(mux.active_panel);
                mark_all_panels_dirty(); // Z-order change affects all panels
                exit_command_mode();
                break;
                
            case 'a':
            case 'A':
                // Send literal Ctrl+A to terminal (like screen does)
                if (active->master_fd >= 0) {
                    write(active->master_fd, "\001", 1); // Ctrl+A
                }
                exit_command_mode();
                break;
                
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
                // Switch to specific panel
                {
                    int panel_num = (ch == '0') ? 0 : ch - '1' + 1;
                    if (panel_num >= 0 && panel_num < mux.panel_count) {
                        mark_panel_dirty(mux.active_panel); // Mark old panel dirty
                        mux.active_panel = panel_num;
                        mark_panel_dirty(mux.active_panel); // Mark new panel dirty
                    }
                    exit_command_mode();
                }
                break;
                
            case 27: // ESC
                // Exit command mode without action
                exit_command_mode();
                break;
                
            default:
                // Unknown command, exit command mode
                exit_command_mode();
                break;
        }
    } else {
        // Normal mode - handle Control key detection and pass through to terminal
        // Use Ctrl+A twice as the trigger (like GNU Screen)
        if (ch == CTRL_KEY('a')) {
            if (mux.ctrl_count == 0) {
                // First Ctrl+A
                mux.ctrl_count = 1;
                return; // Don't send to terminal
            } else if (mux.ctrl_count == 1) {
                // Second Ctrl+A - enter command mode
                enter_command_mode();
                return;
            }
        } else {
            // Reset Control counter for any other key
            mux.ctrl_count = 0;
        }
        
        // Send input to active terminal (skip if it was our trigger key)
        if (active->master_fd >= 0 && ch != CTRL_KEY('a')) {
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
            } else if (ch >= 1 && ch <= 26) {
                // Control characters (including Ctrl+C)
                char c = ch;
                write(active->master_fd, &c, 1);
            } else if (ch == 27) {
                // ESC key
                write(active->master_fd, "\033", 1);
            } else if (ch >= 32 && ch <= 126) {
                // Printable characters
                char c = ch;
                write(active->master_fd, &c, 1);
            }
        }
    }
}

void init_multiplexer() {
    // Initialize multiplexer structure
    memset(&mux, 0, sizeof(multiplexer_t));
    mux.should_quit = 0;
    mux.mode = MODE_NORMAL;
    mux.ctrl_count = 0;
    mux.force_full_redraw = true;
    mux.status_line_dirty = true;
    
    // Initialize locale for UTF-8 support
    setlocale(LC_ALL, "");
    
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
        
        // Initialize colorful color pairs
        init_pair(8, COLOR_RED, -1);
        init_pair(9, COLOR_GREEN, -1);
        init_pair(10, COLOR_YELLOW, -1);
        init_pair(11, COLOR_BLUE, -1);
        init_pair(12, COLOR_MAGENTA, -1);
        init_pair(13, COLOR_CYAN, -1);
        init_pair(14, COLOR_WHITE, -1);
        init_pair(15, COLOR_BLACK, -1);
        
        // Additional color combinations for the background pattern
        if (COLORS >= 16) {
            // Try to use bright colors if available
            init_pair(16, COLOR_RED, COLOR_BLACK);
            init_pair(17, COLOR_GREEN, COLOR_BLACK);
            init_pair(18, COLOR_YELLOW, COLOR_BLACK);
            init_pair(19, COLOR_BLUE, COLOR_BLACK);
            init_pair(20, COLOR_MAGENTA, COLOR_BLACK);
            init_pair(21, COLOR_CYAN, COLOR_BLACK);
        }
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
    
    // Create initial main panel (centered, similar to overlay size)
    mux.panel_count = 1;
    mux.active_panel = 0;
    
    // Calculate main panel size and position (centered, 70% of screen size for better visibility)
    int panel_width = (mux.screen_width * 7) / 10;
    int panel_height = (mux.screen_height * 7) / 10;
    int panel_x = (mux.screen_width - panel_width) / 2;
    int panel_y = (mux.screen_height - panel_height) / 2;
    
    // Ensure minimum size
    if (panel_width < 30) panel_width = 30;
    if (panel_height < 15) panel_height = 15;
    
    // Ensure it fits on screen
    if (panel_x < 0) panel_x = 0;
    if (panel_y < 0) panel_y = 0;
    if (panel_x + panel_width > mux.screen_width) {
        panel_width = mux.screen_width - panel_x;
    }
    if (panel_y + panel_height > mux.screen_height - 1) { // Leave space for status line
        panel_height = mux.screen_height - 1 - panel_y;
    }
    
    if (create_terminal_panel(&mux.panels[0], panel_x, panel_y, 
                             panel_width, panel_height, PANEL_TYPE_MAIN) == -1) {
        endwin();
        fprintf(stderr, "Failed to create main panel\n");
        exit(1);
    }
    
    // Set up initial panel type and z-order
    mux.panel_types[0] = PANEL_TYPE_MAIN;
    mux.panel_z_order[0] = 0;
    
    // Mark initial panel as dirty for first draw
    mark_panel_dirty(0);
    mux.force_full_redraw = true; // Ensure background is drawn initially
    
    // Initial draw with background pattern
    clear();
    draw_background_pattern();
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
        
        // Set timeout for select - reduced for better responsiveness
        timeout.tv_sec = 0;
        timeout.tv_usec = 16667; // ~60 FPS (16.67ms)
        
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
        
        // Optimized rendering - only redraw dirty panels
        bool any_panel_dirty = mux.force_full_redraw;
        for (int i = 0; i < mux.panel_count; i++) {
            if (mux.panel_dirty[i]) {
                any_panel_dirty = true;
                break;
            }
        }
        
        if (any_panel_dirty) {
            // If force_full_redraw, clear screen and draw background pattern
            if (mux.force_full_redraw) {
                clear();
                draw_background_pattern();
            }
            
            // Create array of panel indices sorted by z-order
            int sorted_panels[MAX_PANELS];
            for (int i = 0; i < mux.panel_count; i++) {
                sorted_panels[i] = i;
            }
            
            // Simple bubble sort by z-order (low to high)
            for (int i = 0; i < mux.panel_count - 1; i++) {
                for (int j = 0; j < mux.panel_count - 1 - i; j++) {
                    if (mux.panel_z_order[sorted_panels[j]] > mux.panel_z_order[sorted_panels[j + 1]]) {
                        int temp = sorted_panels[j];
                        sorted_panels[j] = sorted_panels[j + 1];
                        sorted_panels[j + 1] = temp;
                    }
                }
            }
            
            // Draw panels in z-order, but only if dirty or force redraw
            for (int i = 0; i < mux.panel_count; i++) {
                int panel_idx = sorted_panels[i];
                if (mux.panels[panel_idx].active && 
                    (mux.panel_dirty[panel_idx] || mux.force_full_redraw)) {
                    draw_panel(&mux.panels[panel_idx], panel_idx);
                    mux.panel_dirty[panel_idx] = false; // Clear dirty flag
                }
            }
            
            mux.force_full_redraw = false;
        }
        
        // Status line - only redraw if dirty
        bool status_was_dirty = mux.status_line_dirty;
        if (mux.status_line_dirty) {
            // Clear the status line first
            move(mux.screen_height - 1, 0);
            clrtoeol();
            
            if (mux.mode == MODE_COMMAND) {
                // Colorful command mode status
                attron(COLOR_PAIR(10) | A_REVERSE | A_BOLD);
                mvprintw(mux.screen_height - 1, 0, 
                        " ⚡ COMMAND MODE ⚡ | q:quit | n:next | p:prev | c:create | x:close | f:front | 0-7:panel | ESC:cancel ");
                attroff(COLOR_PAIR(10) | A_REVERSE | A_BOLD);
            } else {
                // Status line with emojis and colors
                int status_color = (mux.panel_types[mux.active_panel] == PANEL_TYPE_OVERLAY) ? 12 : 9;
                attron(COLOR_PAIR(status_color));
                
                if (mux.panel_types[mux.active_panel] == PANEL_TYPE_OVERLAY) {
                    mvprintw(mux.screen_height - 1, 0, 
                            "✨ %s %d ✨ | Ctrl+A Ctrl+A: command mode", 
                            "Overlay", mux.active_panel);
                } else {
                    mvprintw(mux.screen_height - 1, 0, 
                            "🖥️  %s 🖥️  | Ctrl+A Ctrl+A: command mode", 
                            "Main Terminal");
                }
                attroff(COLOR_PAIR(status_color));
            }
            mux.status_line_dirty = false;
        }
        
        // Only refresh if something was drawn
        if (any_panel_dirty || status_was_dirty) {
            doupdate(); // More efficient than refresh() when using wnoutrefresh()
        }
    }
    
    cleanup_multiplexer();
    return 0;
}