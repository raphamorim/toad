#include "vte_parser.h"
#include <string.h>
#include <stdlib.h>

// Color mapping
int ansi_to_ncurses_color(int ansi_color) {
    switch (ansi_color) {
        case 0: return COLOR_BLACK;
        case 1: return COLOR_RED;
        case 2: return COLOR_GREEN;
        case 3: return COLOR_YELLOW;
        case 4: return COLOR_BLUE;
        case 5: return COLOR_MAGENTA;
        case 6: return COLOR_CYAN;
        case 7: return COLOR_WHITE;
        default: return -1; // Default color
    }
}

void vte_parser_init(vte_parser_t *parser) {
    memset(parser, 0, sizeof(vte_parser_t));
    parser->state = VTE_STATE_GROUND;
    parser->fg_color = -1; // Default foreground
    parser->bg_color = -1; // Default background
    parser->attrs = A_NORMAL;
}

static void vte_reset_params(vte_parser_t *parser) {
    parser->param_count = 0;
    parser->current_param = 0;
    parser->intermediate_count = 0;
    memset(parser->params, 0, sizeof(parser->params));
    memset(parser->intermediate, 0, sizeof(parser->intermediate));
}

static int vte_get_param(vte_parser_t *parser, int index, int default_val) {
    if (index >= parser->param_count || parser->params[index][0] == '\0') {
        return default_val;
    }
    return atoi(parser->params[index]);
}

static void vte_handle_csi(terminal_panel_t *panel, char final_char) {
    vte_parser_t *parser = &panel->parser;
    
    switch (final_char) {
        case 'm': { // SGR - Select Graphic Rendition
            if (parser->param_count == 0) {
                // Reset to defaults
                parser->fg_color = -1;
                parser->bg_color = -1;
                parser->attrs = A_NORMAL;
                break;
            }
            
            for (int i = 0; i < parser->param_count; i++) {
                int param = vte_get_param(parser, i, 0);
                
                switch (param) {
                    case 0: // Reset
                        parser->fg_color = -1;
                        parser->bg_color = -1;
                        parser->attrs = A_NORMAL;
                        break;
                    case 1: // Bold
                        parser->attrs |= A_BOLD;
                        break;
                    case 4: // Underline
                        parser->attrs |= A_UNDERLINE;
                        break;
                    case 7: // Reverse
                        parser->attrs |= A_REVERSE;
                        break;
                    case 22: // Normal intensity
                        parser->attrs &= ~A_BOLD;
                        break;
                    case 24: // No underline
                        parser->attrs &= ~A_UNDERLINE;
                        break;
                    case 27: // No reverse
                        parser->attrs &= ~A_REVERSE;
                        break;
                    default:
                        if (param >= 30 && param <= 37) {
                            parser->fg_color = ansi_to_ncurses_color(param - 30);
                        } else if (param >= 40 && param <= 47) {
                            parser->bg_color = ansi_to_ncurses_color(param - 40);
                        } else if (param >= 90 && param <= 97) {
                            parser->fg_color = ansi_to_ncurses_color(param - 90);
                            parser->attrs |= A_BOLD;
                        } else if (param >= 100 && param <= 107) {
                            parser->bg_color = ansi_to_ncurses_color(param - 100);
                        }
                        break;
                }
            }
            break;
        }
        case 'H': // CUP - Cursor Position
        case 'f': { // HVP - Horizontal and Vertical Position
            int row = vte_get_param(parser, 0, 1) - 1;
            int col = vte_get_param(parser, 1, 1) - 1;
            
            if (row >= 0 && row < panel->screen_height) {
                panel->cursor_y = row;
            }
            if (col >= 0 && col < panel->screen_width) {
                panel->cursor_x = col;
            }
            break;
        }
        case 'A': { // CUU - Cursor Up
            int count = vte_get_param(parser, 0, 1);
            panel->cursor_y = (panel->cursor_y - count < 0) ? 0 : panel->cursor_y - count;
            break;
        }
        case 'B': { // CUD - Cursor Down
            int count = vte_get_param(parser, 0, 1);
            panel->cursor_y = (panel->cursor_y + count >= panel->screen_height) ? 
                             panel->screen_height - 1 : panel->cursor_y + count;
            break;
        }
        case 'C': { // CUF - Cursor Forward
            int count = vte_get_param(parser, 0, 1);
            panel->cursor_x = (panel->cursor_x + count >= panel->screen_width) ? 
                             panel->screen_width - 1 : panel->cursor_x + count;
            break;
        }
        case 'D': { // CUB - Cursor Back
            int count = vte_get_param(parser, 0, 1);
            panel->cursor_x = (panel->cursor_x - count < 0) ? 0 : panel->cursor_x - count;
            break;
        }
        case 'J': { // ED - Erase in Display
            int param = vte_get_param(parser, 0, 0);
            if (param == 2) { // Clear entire screen
                for (int y = 0; y < panel->screen_height; y++) {
                    for (int x = 0; x < panel->screen_width; x++) {
                        panel->screen[y][x].ch = ' ';
                        panel->screen[y][x].fg_color = -1;
                        panel->screen[y][x].bg_color = -1;
                        panel->screen[y][x].attrs = A_NORMAL;
                    }
                }
            }
            break;
        }
        case 'K': { // EL - Erase in Line
            int param = vte_get_param(parser, 0, 0);
            if (param == 0) { // Clear from cursor to end of line
                for (int x = panel->cursor_x; x < panel->screen_width; x++) {
                    panel->screen[panel->cursor_y][x].ch = ' ';
                    panel->screen[panel->cursor_y][x].fg_color = -1;
                    panel->screen[panel->cursor_y][x].bg_color = -1;
                    panel->screen[panel->cursor_y][x].attrs = A_NORMAL;
                }
            }
            break;
        }
    }
}

static void vte_put_char(terminal_panel_t *panel, char ch) {
    if (panel->cursor_y >= 0 && panel->cursor_y < panel->screen_height &&
        panel->cursor_x >= 0 && panel->cursor_x < panel->screen_width) {
        
        terminal_cell_t *cell = &panel->screen[panel->cursor_y][panel->cursor_x];
        cell->ch = ch;
        cell->fg_color = panel->parser.fg_color;
        cell->bg_color = panel->parser.bg_color;
        cell->attrs = panel->parser.attrs;
        
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
                    panel->screen[panel->screen_height - 1][x].ch = ' ';
                    panel->screen[panel->screen_height - 1][x].fg_color = -1;
                    panel->screen[panel->screen_height - 1][x].bg_color = -1;
                    panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                }
                panel->cursor_y = panel->screen_height - 1;
            }
        }
    }
}

void vte_parser_feed(terminal_panel_t *panel, const char *data, size_t len) {
    vte_parser_t *parser = &panel->parser;
    
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = data[i];
        
        switch (parser->state) {
            case VTE_STATE_GROUND:
                if (ch == '\033') {
                    parser->state = VTE_STATE_ESCAPE;
                } else if (ch == '\n') {
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
                            panel->screen[panel->screen_height - 1][x].ch = ' ';
                            panel->screen[panel->screen_height - 1][x].fg_color = -1;
                            panel->screen[panel->screen_height - 1][x].bg_color = -1;
                            panel->screen[panel->screen_height - 1][x].attrs = A_NORMAL;
                        }
                        panel->cursor_y = panel->screen_height - 1;
                    }
                } else if (ch == '\r') {
                    panel->cursor_x = 0;
                } else if (ch == '\b') {
                    if (panel->cursor_x > 0) {
                        panel->cursor_x--;
                    }
                } else if (ch >= 32 && ch <= 126) {
                    vte_put_char(panel, ch);
                }
                break;
                
            case VTE_STATE_ESCAPE:
                if (ch == '[') {
                    parser->state = VTE_STATE_CSI_ENTRY;
                    vte_reset_params(parser);
                } else {
                    parser->state = VTE_STATE_GROUND;
                }
                break;
                
            case VTE_STATE_CSI_ENTRY:
                if (ch >= '0' && ch <= '9') {
                    parser->state = VTE_STATE_CSI_PARAM;
                    parser->params[0][0] = ch;
                    parser->params[0][1] = '\0';
                    parser->param_count = 1;
                } else if (ch >= 0x40 && ch <= 0x7E) {
                    vte_handle_csi(panel, ch);
                    parser->state = VTE_STATE_GROUND;
                } else {
                    parser->state = VTE_STATE_GROUND;
                }
                break;
                
            case VTE_STATE_CSI_PARAM:
                if (ch >= '0' && ch <= '9') {
                    int len = strlen(parser->params[parser->param_count - 1]);
                    if (len < 15) {
                        parser->params[parser->param_count - 1][len] = ch;
                        parser->params[parser->param_count - 1][len + 1] = '\0';
                    }
                } else if (ch == ';') {
                    if (parser->param_count < MAX_PARAMS) {
                        parser->param_count++;
                        parser->params[parser->param_count - 1][0] = '\0';
                    }
                } else if (ch >= 0x40 && ch <= 0x7E) {
                    vte_handle_csi(panel, ch);
                    parser->state = VTE_STATE_GROUND;
                } else {
                    parser->state = VTE_STATE_GROUND;
                }
                break;
                
            default:
                parser->state = VTE_STATE_GROUND;
                break;
        }
    }
}