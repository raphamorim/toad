#include "vte_parser.h"
#include <ncurses.h>
#include <string.h>

// Terminal-specific perform implementation
static void terminal_print(terminal_panel_t *panel, uint32_t codepoint) {
    if (panel->cursor_y >= 0 && panel->cursor_y < panel->screen_height &&
        panel->cursor_x >= 0 && panel->cursor_x < panel->screen_width) {
        
        terminal_cell_t *cell = &panel->screen[panel->cursor_y][panel->cursor_x];
        cell->codepoint = codepoint;
        cell->fg_color = panel->fg_color;
        cell->bg_color = panel->bg_color;
        cell->attrs = panel->attrs;
        
        panel->cursor_x++;
        if (panel->cursor_x >= panel->screen_width) {
            panel->cursor_x = 0;
            panel->cursor_y++;
            if (panel->cursor_y >= panel->screen_height) {
                // Scroll up
                for (int y = 0; y < panel->screen_height - 1; y++) {
                    memcpy(panel->screen[y], panel->screen[y + 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear last line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[panel->screen_height - 1][x].codepoint = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
                panel->cursor_y = panel->screen_height - 1;
            }
        }
    }
}

static void terminal_execute(terminal_panel_t *panel, uint8_t byte) {
    switch (byte) {
        case '\n':
            panel->cursor_x = 0;
            panel->cursor_y++;
            if (panel->cursor_y >= panel->screen_height) {
                // Scroll up
                for (int y = 0; y < panel->screen_height - 1; y++) {
                    memcpy(panel->screen[y], panel->screen[y + 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear last line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[panel->screen_height - 1][x].codepoint = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
                panel->cursor_y = panel->screen_height - 1;
            }
            break;
        case '\r':
            panel->cursor_x = 0;
            break;
        case '\b':
            if (panel->cursor_x > 0) {
                panel->cursor_x--;
            }
            break;
        case '\t':
            // Tab to next 8-column boundary
            panel->cursor_x = ((panel->cursor_x + 8) / 8) * 8;
            if (panel->cursor_x >= panel->screen_width) {
                panel->cursor_x = panel->screen_width - 1;
            }
            break;
    }
}

static void terminal_csi_dispatch(terminal_panel_t *panel, const vte_params_t *params,
                                 const uint8_t *intermediates, size_t intermediate_len,
                                 bool ignore, char action) {
    if (ignore) return;
    
    switch (action) {
        case 'm': { // SGR - Select Graphic Rendition
            if (vte_params_len(params) == 0) {
                // Reset to defaults
                panel->fg_color = -1;
                panel->bg_color = -1;
                panel->attrs = A_NORMAL;
                break;
            }
            
            for (size_t i = 0; i < vte_params_len(params); i++) {
                size_t subparam_count;
                const uint16_t *param_values = vte_params_get(params, i, &subparam_count);
                if (!param_values) continue;
                
                for (size_t j = 0; j < subparam_count; j++) {
                    uint16_t param = param_values[j];
                    
                    switch (param) {
                        case 0: // Reset
                            panel->fg_color = -1;
                            panel->bg_color = -1;
                            panel->attrs = A_NORMAL;
                            break;
                        case 1: // Bold
                            panel->attrs |= A_BOLD;
                            break;
                        case 4: // Underline
                            panel->attrs |= A_UNDERLINE;
                            break;
                        case 7: // Reverse
                            panel->attrs |= A_REVERSE;
                            break;
                        case 22: // Normal intensity
                            panel->attrs &= ~A_BOLD;
                            break;
                        case 24: // No underline
                            panel->attrs &= ~A_UNDERLINE;
                            break;
                        case 27: // No reverse
                            panel->attrs &= ~A_REVERSE;
                            break;
                        case 39: // Default foreground color
                            panel->fg_color = -1;
                            break;
                        case 49: // Default background color
                            panel->bg_color = -1;
                            break;
                        default:
                            if (param >= 30 && param <= 37) {
                                panel->fg_color = ansi_to_ncurses_color(param - 30);
                            } else if (param >= 40 && param <= 47) {
                                panel->bg_color = ansi_to_ncurses_color(param - 40);
                            } else if (param >= 90 && param <= 97) {
                                panel->fg_color = ansi_to_ncurses_color(param - 90);
                                panel->attrs |= A_BOLD;
                            } else if (param >= 100 && param <= 107) {
                                panel->bg_color = ansi_to_ncurses_color(param - 100);
                            }
                            break;
                    }
                }
            }
            break;
        }
        case 'H': // CUP - Cursor Position
        case 'f': { // HVP - Horizontal and Vertical Position
            uint16_t row = vte_params_get_single(params, 0, 1) - 1;
            uint16_t col = vte_params_get_single(params, 1, 1) - 1;
            
            if (row < panel->screen_height) {
                panel->cursor_y = row;
            }
            if (col < panel->screen_width) {
                panel->cursor_x = col;
            }
            break;
        }
        case 'A': { // CUU - Cursor Up
            uint16_t count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y < count) ? 0 : panel->cursor_y - count;
            break;
        }
        case 'B': { // CUD - Cursor Down
            uint16_t count = vte_params_get_single(params, 0, 1);
            panel->cursor_y = (panel->cursor_y + count >= panel->screen_height) ? 
                             panel->screen_height - 1 : panel->cursor_y + count;
            break;
        }
        case 'C': { // CUF - Cursor Forward
            uint16_t count = vte_params_get_single(params, 0, 1);
            panel->cursor_x = (panel->cursor_x + count >= panel->screen_width) ? 
                             panel->screen_width - 1 : panel->cursor_x + count;
            break;
        }
        case 'D': { // CUB - Cursor Back
            uint16_t count = vte_params_get_single(params, 0, 1);
            panel->cursor_x = (panel->cursor_x < count) ? 0 : panel->cursor_x - count;
            break;
        }
        case 'J': { // ED - Erase in Display
            uint16_t param = vte_params_get_single(params, 0, 0);
            switch (param) {
                case 0: // Clear from cursor to end of screen
                    // Clear from cursor to end of line
                    for (int x = panel->cursor_x; x < panel->screen_width; x++) {
                        panel->screen[panel->cursor_y][x].codepoint = ' ';
                        panel->screen[panel->cursor_y][x].fg_color = -1;
                        panel->screen[panel->cursor_y][x].bg_color = -1;
                        panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                    }
                    // Clear all lines below
                    for (int y = panel->cursor_y + 1; y < panel->screen_height; y++) {
                        for (int x = 0; x < panel->screen_width; x++) {
                            panel->screen[y][x].codepoint = ' ';
                            panel->screen[y][x].fg_color = -1;
                            panel->screen[y][x].bg_color = -1;
                            panel->screen[y][x].attrs = A_NORMAL;
                        }
                    }
                    break;
                case 1: // Clear from beginning of screen to cursor
                    // Clear all lines above
                    for (int y = 0; y < panel->cursor_y; y++) {
                        for (int x = 0; x < panel->screen_width; x++) {
                            panel->screen[y][x].codepoint = ' ';
                            panel->screen[y][x].fg_color = -1;
                            panel->screen[y][x].bg_color = -1;
                            panel->screen[y][x].attrs = A_NORMAL;
                        }
                    }
                    // Clear from beginning of line to cursor
                    for (int x = 0; x <= panel->cursor_x; x++) {
                        panel->screen[panel->cursor_y][x].codepoint = ' ';
                        panel->screen[panel->cursor_y][x].fg_color = -1;
                        panel->screen[panel->cursor_y][x].bg_color = -1;
                        panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                    }
                    break;
                case 2: // Clear entire screen
                case 3: // Clear entire screen and scrollback
                    for (int y = 0; y < panel->screen_height; y++) {
                        for (int x = 0; x < panel->screen_width; x++) {
                            panel->screen[y][x].codepoint = ' ';
                            panel->screen[y][x].fg_color = -1;
                            panel->screen[y][x].bg_color = -1;
                            panel->screen[y][x].attrs = A_NORMAL;
                        }
                    }
                    break;
            }
            break;
        }
        case 'K': { // EL - Erase in Line
            uint16_t param = vte_params_get_single(params, 0, 0);
            switch (param) {
                case 0: // Clear from cursor to end of line
                    for (int x = panel->cursor_x; x < panel->screen_width; x++) {
                        panel->screen[panel->cursor_y][x].codepoint = ' ';
                        panel->screen[panel->cursor_y][x].fg_color = -1;
                        panel->screen[panel->cursor_y][x].bg_color = -1;
                        panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                    }
                    break;
                case 1: // Clear from beginning of line to cursor
                    for (int x = 0; x <= panel->cursor_x; x++) {
                        panel->screen[panel->cursor_y][x].codepoint = ' ';
                        panel->screen[panel->cursor_y][x].fg_color = -1;
                        panel->screen[panel->cursor_y][x].bg_color = -1;
                        panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                    }
                    break;
                case 2: // Clear entire line
                    for (int x = 0; x < panel->screen_width; x++) {
                        panel->screen[panel->cursor_y][x].codepoint = ' ';
                        panel->screen[panel->cursor_y][x].fg_color = -1;
                        panel->screen[panel->cursor_y][x].bg_color = -1;
                        panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                    }
                    break;
            }
            break;
        }
        case 'S': { // SU - Scroll Up
            uint16_t count = vte_params_get_single(params, 0, 1);
            for (uint16_t i = 0; i < count && i < panel->screen_height; i++) {
                // Scroll up one line
                for (int y = 0; y < panel->screen_height - 1; y++) {
                    memcpy(panel->screen[y], panel->screen[y + 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear last line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[panel->screen_height - 1][x].codepoint = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
            }
            break;
        }
        case 'T': { // SD - Scroll Down
            uint16_t count = vte_params_get_single(params, 0, 1);
            for (uint16_t i = 0; i < count && i < panel->screen_height; i++) {
                // Scroll down one line
                for (int y = panel->screen_height - 1; y > 0; y--) {
                    memcpy(panel->screen[y], panel->screen[y - 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear first line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[0][x].codepoint = ' ';
                    panel->screen[0][x].fg_color = -1;
                    panel->screen[0][x].bg_color = -1;
                    panel->screen[0][x].attrs = A_NORMAL;
                }
            }
            break;
        }
    }
}

static void terminal_esc_dispatch(terminal_panel_t *panel, const uint8_t *intermediates,
                                 size_t intermediate_len, bool ignore, uint8_t byte) {
    if (ignore) return;
    
    // Handle common escape sequences
    switch (byte) {
        case 'D': // IND - Index (move cursor down, scroll if at bottom)
            panel->cursor_y++;
            if (panel->cursor_y >= panel->screen_height) {
                // Scroll up
                for (int y = 0; y < panel->screen_height - 1; y++) {
                    memcpy(panel->screen[y], panel->screen[y + 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear last line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[panel->screen_height - 1][x].codepoint = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
                panel->cursor_y = panel->screen_height - 1;
            }
            break;
        case 'M': // RI - Reverse Index (move cursor up, scroll if at top)
            if (panel->cursor_y > 0) {
                panel->cursor_y--;
            } else {
                // Scroll down
                for (int y = panel->screen_height - 1; y > 0; y--) {
                    memcpy(panel->screen[y], panel->screen[y - 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear first line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[0][x].codepoint = ' ';
                    panel->screen[0][x].fg_color = -1;
                    panel->screen[0][x].bg_color = -1;
                    panel->screen[0][x].attrs = A_NORMAL;
                }
            }
            break;
        case 'E': // NEL - Next Line
            panel->cursor_x = 0;
            panel->cursor_y++;
            if (panel->cursor_y >= panel->screen_height) {
                // Scroll up
                for (int y = 0; y < panel->screen_height - 1; y++) {
                    memcpy(panel->screen[y], panel->screen[y + 1], 
                           panel->screen_width * sizeof(terminal_cell_t));
                }
                // Clear last line
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[panel->screen_height - 1][x].codepoint = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
                panel->cursor_y = panel->screen_height - 1;
            }
            break;
        case 'c': // RIS - Reset to Initial State
            // Reset terminal state
            panel->fg_color = -1;
            panel->bg_color = -1;
            panel->attrs = A_NORMAL;
            panel->cursor_x = 0;
            panel->cursor_y = 0;
            // Clear screen
            for (int y = 0; y < panel->screen_height; y++) {
                for (int x = 0; x < panel->screen_width; x++) {
                    panel->screen[y][x].codepoint = ' ';
                    panel->screen[y][x].fg_color = -1;
                    panel->screen[y][x].bg_color = -1;
                    panel->screen[y][x].attrs = A_NORMAL;
                }
            }
            break;
    }
}

static void terminal_osc_dispatch(terminal_panel_t *panel, const uint8_t *const *params,
                                 const size_t *param_lens, size_t num_params, bool bell_terminated) {
    if (num_params < 1) return;
    
    // Parse OSC command number
    uint16_t cmd = 0;
    for (size_t i = 0; i < param_lens[0] && i < 5; i++) {
        if (params[0][i] >= '0' && params[0][i] <= '9') {
            cmd = cmd * 10 + (params[0][i] - '0');
        } else {
            break;
        }
    }
    
    switch (cmd) {
        case 0: // Set window title and icon name
        case 2: // Set window title
            // Could implement window title setting here
            break;
        case 1: // Set icon name
            // Could implement icon name setting here
            break;
        default:
            // Unknown OSC command
            break;
    }
}

// Terminal perform implementation
const vte_perform_t terminal_perform = {
    .print = terminal_print,
    .execute = terminal_execute,
    .csi_dispatch = terminal_csi_dispatch,
    .esc_dispatch = terminal_esc_dispatch,
    .osc_dispatch = terminal_osc_dispatch,
    .hook = NULL,
    .put = NULL,
    .unhook = NULL
};

// Compatibility function for existing code
void vte_parser_feed(terminal_panel_t *panel, const char *data, size_t len) {
    // Ensure the panel has the terminal perform implementation
    if (!panel->perform.print) {
        panel->perform = terminal_perform;
    }
    
    vte_parser_advance(&panel->parser, panel, (const uint8_t *)data, len);
}