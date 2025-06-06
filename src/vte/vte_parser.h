#ifndef VTE_PARSER_H
#define VTE_PARSER_H

#include <ncurses.h>

#define MAX_PARAMS 16

// VTE parser states
typedef enum {
    VTE_STATE_GROUND,
    VTE_STATE_ESCAPE,
    VTE_STATE_CSI_ENTRY,
    VTE_STATE_CSI_PARAM,
    VTE_STATE_CSI_INTERMEDIATE,
    VTE_STATE_OSC_STRING
} vte_state_t;

// Terminal cell structure
typedef struct {
    char ch;
    int fg_color;
    int bg_color;
    int attrs;
} terminal_cell_t;

// VTE parser structure
typedef struct {
    vte_state_t state;
    char params[MAX_PARAMS][16];
    int param_count;
    int current_param;
    char intermediate[8];
    int intermediate_count;
    
    // Current text attributes
    int fg_color;
    int bg_color;
    int attrs;
} vte_parser_t;

// Terminal panel structure (forward declaration)
typedef struct {
    WINDOW *win;  // WINDOW pointer
    int master_fd;
    int child_pid;
    terminal_cell_t **screen;
    int scroll_offset;
    int active;
    int width, height;
    int start_x, start_y;
    int cursor_x, cursor_y;
    int screen_width, screen_height;
    vte_parser_t parser;
} terminal_panel_t;

// Function declarations
void vte_parser_init(vte_parser_t *parser);
void vte_parser_feed(terminal_panel_t *panel, const char *data, size_t len);
int ansi_to_ncurses_color(int ansi_color);

#endif // VTE_PARSER_H