#ifndef VTE_PARSER_H
#define VTE_PARSER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define VTE_MAX_PARAMS 32
#define VTE_MAX_INTERMEDIATES 2
#define VTE_MAX_OSC_RAW 1024
#define VTE_MAX_OSC_PARAMS 16

// VTE parser states based on Paul Williams' state machine
typedef enum {
    VTE_STATE_GROUND,
    VTE_STATE_ESCAPE,
    VTE_STATE_ESCAPE_INTERMEDIATE,
    VTE_STATE_CSI_ENTRY,
    VTE_STATE_CSI_PARAM,
    VTE_STATE_CSI_INTERMEDIATE,
    VTE_STATE_CSI_IGNORE,
    VTE_STATE_DCS_ENTRY,
    VTE_STATE_DCS_PARAM,
    VTE_STATE_DCS_INTERMEDIATE,
    VTE_STATE_DCS_PASSTHROUGH,
    VTE_STATE_DCS_IGNORE,
    VTE_STATE_OSC_STRING,
    VTE_STATE_SOS_PM_APC_STRING
} vte_state_t;

// Parameter structure supporting subparameters
typedef struct {
    uint16_t params[VTE_MAX_PARAMS];
    uint8_t subparams[VTE_MAX_PARAMS];  // Number of subparams for each param
    uint8_t current_subparams;
    size_t len;
} vte_params_t;

// OSC parameter tracking
typedef struct {
    size_t start;
    size_t end;
} vte_osc_param_t;

// VTE parser structure
typedef struct {
    vte_state_t state;
    
    // Parameter handling
    vte_params_t params;
    uint16_t current_param;
    
    // Intermediate characters
    uint8_t intermediates[VTE_MAX_INTERMEDIATES];
    size_t intermediate_idx;
    
    // OSC string handling
    uint8_t osc_raw[VTE_MAX_OSC_RAW];
    size_t osc_raw_len;
    vte_osc_param_t osc_params[VTE_MAX_OSC_PARAMS];
    size_t osc_num_params;
    
    // UTF-8 handling
    uint8_t partial_utf8[4];
    size_t partial_utf8_len;
    
    // State flags
    bool ignoring;
} vte_parser_t;

// Forward declaration for terminal panel
typedef struct terminal_panel terminal_panel_t;

// Perform trait - callbacks for VTE actions
typedef struct {
    // Print a character to the terminal
    void (*print)(terminal_panel_t *panel, uint32_t codepoint);
    
    // Execute a C0/C1 control character
    void (*execute)(terminal_panel_t *panel, uint8_t byte);
    
    // CSI sequence dispatch
    void (*csi_dispatch)(terminal_panel_t *panel, const vte_params_t *params, 
                        const uint8_t *intermediates, size_t intermediate_len,
                        bool ignore, char action);
    
    // ESC sequence dispatch
    void (*esc_dispatch)(terminal_panel_t *panel, const uint8_t *intermediates, 
                        size_t intermediate_len, bool ignore, uint8_t byte);
    
    // OSC sequence dispatch
    void (*osc_dispatch)(terminal_panel_t *panel, const uint8_t *const *params, 
                        const size_t *param_lens, size_t num_params, bool bell_terminated);
    
    // DCS hooks
    void (*hook)(terminal_panel_t *panel, const vte_params_t *params,
                const uint8_t *intermediates, size_t intermediate_len,
                bool ignore, char action);
    void (*put)(terminal_panel_t *panel, uint8_t byte);
    void (*unhook)(terminal_panel_t *panel);
} vte_perform_t;

// Terminal cell structure
typedef struct {
    uint32_t codepoint;
    int fg_color;
    int bg_color;
    int attrs;
} terminal_cell_t;

// Terminal panel structure
struct terminal_panel {
    void *win;  // WINDOW pointer (void* to avoid ncurses dependency in header)
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
    vte_perform_t perform;
    
    // Current text attributes
    int fg_color;
    int bg_color;
    int attrs;
    
    // Character set state
    bool dec_special_charset;  // True when using DEC special character set
};

// Function declarations
void vte_parser_init(vte_parser_t *parser);
void vte_parser_advance(vte_parser_t *parser, terminal_panel_t *panel, 
                       const uint8_t *data, size_t len);
size_t vte_parser_advance_until_terminated(vte_parser_t *parser, terminal_panel_t *panel,
                                          const uint8_t *data, size_t len);

// Parameter utilities
void vte_params_init(vte_params_t *params);
void vte_params_clear(vte_params_t *params);
void vte_params_push(vte_params_t *params, uint16_t value);
void vte_params_extend(vte_params_t *params, uint16_t value);
bool vte_params_is_full(const vte_params_t *params);
size_t vte_params_len(const vte_params_t *params);
const uint16_t *vte_params_get(const vte_params_t *params, size_t index, size_t *subparam_count);
uint16_t vte_params_get_single(const vte_params_t *params, size_t index, uint16_t default_val);

// UTF-8 utilities
bool vte_is_utf8_continuation(uint8_t byte);
size_t vte_utf8_char_len(uint8_t first_byte);
uint32_t vte_utf8_decode(const uint8_t *bytes, size_t len);

// Default perform implementation
extern const vte_perform_t vte_default_perform;

// Terminal-specific perform implementation
extern const vte_perform_t terminal_perform;

// Compatibility function for existing code
void vte_parser_feed(terminal_panel_t *panel, const char *data, size_t len);

// Color mapping
int ansi_to_ncurses_color(int ansi_color);

#endif // VTE_PARSER_H