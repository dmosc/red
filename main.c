/*** Includes ***/
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** Definitions ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define TAB_STOP 4
#define CTRL_KEY(k) ((k) & 0x1f)
#define BUFFER_INIT {NULL, 0}
#define enum_to_string(m) #m
#define stringify(m) enum_to_string(m)
enum KEYS {
    BACKSPACE = 127,
    ESCAPE = 27,
    UP = 1000,
    DOWN,
    RIGHT,
    LEFT,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    READ_MODE,
    EDIT_MODE
};
enum HIGHLIGHTS {
    HL_DEFAULT = 0,
    HL_NUMBER
};

/*** Structs ***/
typedef struct document_row {
    char *content, *render_content;
    int size, render_size;
    unsigned char *highlight;
} document_row;

struct editor_config {
    int cursor_x, cursor_y;
    int rows, cols;
    int row_offset, col_offset;
    int document_rows;
    document_row *row;
    char *file_name;
    char status[80];
    time_t status_time;
    int mode;
    struct termios initial_state;
};

struct editor_config EC;

/*** Utils ***/
void clear_and_reposition_cursor() {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
}

/*** Prototypes ***/
void row_append_render(document_row *row);

void insert_char(int c);

void insert_new_line();

void refresh_screen();

void delete_char();

void save_file();

/*** Buffer printer ***/
struct buffer {
    char *content;
    int size;
};

void buffer_append(struct buffer *buff, const char *s, int size) {
    char *new_content = realloc(buff->content, buff->size + size);

    if (!new_content) return;
    memcpy(&new_content[buff->size], s, size);
    buff->content = new_content;
    buff->size += size;
}

void buffer_drop(struct buffer *buff) {
    free(buff->content);
}

/*** Functions ***/
void editor_exit(const char *s) {
    clear_and_reposition_cursor();

    perror(s);
    exit(1);
}

void editor_disable() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &EC.initial_state) == -1) editor_exit("tcsetattr");
}

void editor_enable() {
    if (tcgetattr(STDIN_FILENO, &EC.initial_state) == -1) editor_exit("tcgetattr");
    atexit(editor_disable);

    struct termios raw = EC.initial_state;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) editor_exit("tcsetattr");
}

int read_key() {
    int read_no;
    char c;

    while ((read_no = read(STDIN_FILENO, &c, 1)) != 1) {
        if (read_no == -1 && errno != EAGAIN) editor_exit("read");
    }

    // Process arrow key sequence to determine cursor direction.
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1':
                            return HOME_KEY;
                        case '3':
                            return DEL_KEY;
                        case '4':
                            return END_KEY;
                        case '5':
                            return PAGE_UP;
                        case '6':
                            return PAGE_DOWN;
                        case '7':
                            return HOME_KEY;
                        case '8':
                            return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A':
                        return UP;
                    case 'B':
                        return DOWN;
                    case 'C':
                        return RIGHT;
                    case 'D':
                        return LEFT;
                    case 'H':
                        return HOME_KEY;
                    case 'F':
                        return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

void editor_init() {
    EC.cursor_x = 0;
    EC.cursor_y = 0;
    EC.row_offset = 0;
    EC.col_offset = 0;
    EC.document_rows = 0;
    EC.row = NULL;
    EC.file_name = NULL;
    EC.status[0] = '\0';
    EC.status_time = 0;
    EC.mode = READ_MODE;

    if (window_size(&EC.rows, &EC.cols) == -1) editor_exit("window_size");
    EC.rows -= 2; // Leave space for status bar and status messages.
}

/*** Syntax highlighting functions ***/
void row_set_syntax(document_row *row) {
    row->highlight = realloc(row->highlight, row->render_size);
    memset(row->highlight, HL_DEFAULT, row->render_size);

    for (int i = 0; i < row->render_size; ++i) {
        if (isdigit(row->render_content[i])) {
            row->highlight[i] = HL_NUMBER;
        }
    }
}

int syntax_to_color_code(int highlight) {
    switch (highlight) {
        case HL_NUMBER:
            return 31;
        default:
            return 39;
    }
}

/*** Output functions ***/
void scroll_window() {
    if (EC.cursor_y < EC.row_offset) // Check if cursor above visible window and scroll up
        EC.row_offset = EC.cursor_y;
    if (EC.cursor_y >= EC.row_offset + EC.rows) // Check if cursor below visible window and scroll down
        EC.row_offset = EC.cursor_y - EC.rows + 1;
    if (EC.cursor_x < EC.col_offset)
        EC.col_offset = EC.cursor_x;
    if (EC.cursor_x >= EC.col_offset + EC.cols)
        EC.col_offset = EC.cursor_x - EC.cols + 1;
}

void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(EC.status, sizeof(EC.status), fmt, ap);
    va_end(ap);
    EC.status_time = time(NULL);
}

void draw_rows(struct buffer *buff) {
    for (int r = 0; r < EC.rows; ++r) {
        int i = r + EC.row_offset;
        if (i >= EC.document_rows) {
            buffer_append(buff, "~", 1);
        } else {
            int size = EC.row[i].render_size - EC.col_offset;
            if (size < 0) size = 0;
            if (size > EC.cols) size = EC.cols;

            char *c = &EC.row[i].render_content[EC.col_offset];
            unsigned char *highlight = &EC.row[i].highlight[EC.col_offset];
            int current_color = -1;
            for (int j = 0; j < size; ++j) {
                if (highlight[j] == HL_DEFAULT) { // Color numbers in red
                    if (current_color != -1) {
                        current_color = -1;
                        buffer_append(buff, "\x1b[39m", 5);
                    }
                    buffer_append(buff, &c[j], 1);
                } else {
                    int color = syntax_to_color_code(highlight[j]);
                    if (color != current_color) {
                        current_color = color;
                        char line[16];
                        int line_size = snprintf(line, sizeof(line), "\x1b[%dm", color);
                        buffer_append(buff, line, line_size);
                    }
                    buffer_append(buff, &c[j], 1);
                }
                buffer_append(buff, "\x1b[39m", 5);
            }
        }

        buffer_append(buff, "\x1b[K", 3);
        buffer_append(buff, "\r\n", 2);
    }
}

void draw_message_bar(struct buffer *buff) {
    buffer_append(buff, "\x1b[K", 3);
    int size = strlen(EC.status);
    if (size > EC.cols) size = EC.cols;
    if (size && time(NULL) - EC.status_time < 5)
        buffer_append(buff, EC.status, size);
}

void draw_status_bar(struct buffer *buff) {
    buffer_append(buff, "\x1b[7m", 4); // Invert colors

    char status[80], *mode;
    switch (EC.mode) {
        case READ_MODE:
            mode = stringify(READ_MODE);
            break;
        case EDIT_MODE:
            mode = stringify(EDIT_MODE);
            break;
    }
    int cols = snprintf(
            status,
            sizeof(status),
            " %.20s - %d lines [%s]",
            EC.file_name ? EC.file_name : "[New document]",
            EC.document_rows,
            mode
    );

    if (cols > EC.cols) cols = EC.cols;
    buffer_append(buff, status, cols);
    while (++cols < EC.cols)
        buffer_append(buff, " ", 1);
    buffer_append(buff, "\x1b[m", 3);
    buffer_append(buff, "\r\n", 2);
}

void refresh_screen() {
    scroll_window();
    struct buffer buff = BUFFER_INIT;

    buffer_append(&buff, "\x1b[?25l", 6);
    buffer_append(&buff, "\x1b[H", 3);

    draw_rows(&buff);
    draw_status_bar(&buff);
    draw_message_bar(&buff);

    char cursor_buff[32];
    snprintf(cursor_buff, sizeof(cursor_buff), "\x1b[%d;%dH", (EC.cursor_y - EC.row_offset) + 1,
             (EC.cursor_x - EC.col_offset) + 1);
    buffer_append(&buff, cursor_buff, strlen(cursor_buff));
    buffer_append(&buff, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buff.content, buff.size);
    buffer_drop(&buff);
}

/*** Input functions ***/
char *show_prompt(char *prompt) {
    size_t size = 128, current_size = 0;
    char *buffer = malloc(size);
    buffer[0] = '\0';

    while (1) {
        set_status(prompt, buffer);
        refresh_screen();

        int c = read_key();
        if (c == '\x1b') { // If user "escapes", the prompt gets cancelled.
            set_status("");
            free(buffer);
            return NULL;
        } else if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (current_size != 0) buffer[--current_size] = '\0';
        } else if (c == '\r') {
            if (current_size != 0) { // When the user presses enter, return the prompt content.
                set_status("");
                return buffer;
            }
        } else if (!iscntrl(c) && c < 128) { // Make sure to avoid reacting to any escape sequence.
            if (current_size == size - 1) {
                size *= 2;
                buffer = realloc(buffer, size);
            }
            buffer[current_size++] = c;
            buffer[current_size] = '\0';
        }
    }
}

void move_cursor(int key) {
    document_row *row = EC.cursor_y >= EC.rows ? NULL : &EC.row[EC.cursor_y];
    switch (key) {
        case UP:
            if (EC.cursor_y != 0) --EC.cursor_y;
            break;
        case DOWN:
            if (EC.cursor_y < EC.document_rows) ++EC.cursor_y;
            break;
        case RIGHT:
            if (row && EC.cursor_x < row->size) {
                ++EC.cursor_x;
            } else if (row && EC.cursor_x == row->size) {
                ++EC.cursor_y;
                EC.cursor_x = 0;
            }
            break;
        case LEFT:
            if (EC.cursor_x != 0) {
                --EC.cursor_x;
            } else if (EC.cursor_y > 0) {
                --EC.cursor_y;
                EC.cursor_x = EC.row[EC.cursor_y].size;
            }
            break;
    }

    row = (EC.cursor_y >= EC.document_rows) ? NULL : &EC.row[EC.cursor_y];
    int size = row ? row->size : 0;
    if (EC.cursor_x > size) {
        EC.cursor_x = size;
    }
}

void process_command() {
    char *request = show_prompt("/%s");
    char *command = strtok(request, " ");

    if (strcmp(command, "save") == 0 || strcmp(command, "s") == 0) { // Save document's current state
        save_file();
    } else if (strcmp(command, "line") == 0 || strcmp(command, "l") == 0 || strcmp(command, "n") == 0) { // Jump to line
        int line = atoi(strtok(NULL, " "));
        EC.cursor_y = line;
    } else if (strcmp(command, "find") == 0 || strcmp(command, "f") == 0) {
        char *query = strtok(NULL, " ");
        struct buffer buff = BUFFER_INIT;

        if (query) {
            while (query != NULL) {
                buffer_append(&buff, query, strlen(query));
                buffer_append(&buff, " ", 1);
                query = strtok(NULL, " ");
            }

            int incidences = 0;
            for (int i = EC.document_rows - 2; i >= 0; --i) {
                document_row *row = &EC.row[i];
                char *match = strstr(row->render_content, buff.content);
                if (match) {
                    EC.cursor_y = i;
                    EC.cursor_x = match - row->render_content;
                    EC.row_offset = EC.document_rows;
                    ++incidences;
                }
            }

            set_status("%d incidences found", incidences);
            free(query);
            buffer_drop(&buff);
        } else {
            set_status("A query is required! - find [a-zA-Z1-9]");
        }
    } else {
        set_status("Command not found! Visit the docs at https://github.com/oscardavidrm/red");
    }
}

void process_key() {
    int c = read_key();
    switch (c) { // Switch between editor modes and I/O operations.
        case CTRL_KEY('r'):
            EC.mode = READ_MODE;
            return;
        case CTRL_KEY('e'):
            EC.mode = EDIT_MODE;
            return;
        case CTRL_KEY('c'):
            process_command();
            return;
        case CTRL_KEY('q'):
            clear_and_reposition_cursor();
            exit(0);
    }

    switch (c) { // Moving cursor and traversing the document
        case HOME_KEY:
            EC.cursor_x = 0;
            return;
        case END_KEY:
            if (EC.cursor_y < EC.document_rows) EC.cursor_x = EC.row[EC.cursor_y].size;
            return;
        case PAGE_UP:
        case PAGE_DOWN: {
            if (c == PAGE_UP) {
                EC.cursor_y = EC.row_offset;
            } else {
                EC.cursor_y = EC.row_offset + EC.document_rows - 1;
                if (EC.cursor_y > EC.document_rows) EC.cursor_y = EC.document_rows;
            }

            int times = EC.rows;
            while (times--)
                move_cursor(c == PAGE_UP ? UP : DOWN);
            return;
        }
        case UP:
        case DOWN:
        case RIGHT:
        case LEFT:
            move_cursor(c);
            return;
    }

    if (EC.mode == EDIT_MODE) { // Edit operations
        switch (c) {
            case '\r':
                insert_new_line();
                break;
            case BACKSPACE:
            case CTRL_KEY('h'):
            case DEL_KEY:
                if (c == DEL_KEY) move_cursor(RIGHT);
                delete_char();
                break;
            case CTRL_KEY('l'):
            default:
                insert_char(c);
                break;
        }
    }
}

/*** File functions ***/
void row_append(int i, char *line, size_t size) {
    if (i < 0 || i > EC.document_rows) return;

    EC.row = realloc(EC.row, sizeof(document_row) * (EC.document_rows + 1));
    memmove(&EC.row[i + 1], &EC.row[i], sizeof(document_row) * (EC.document_rows - i));

    EC.row[i].size = size;
    EC.row[i].content = malloc(size + 1);
    memcpy(EC.row[i].content, line, size);
    EC.row[i].content[size] = '\0';
    EC.row[i].render_size = 0;
    EC.row[i].render_content = NULL;
    EC.row[i].highlight = NULL;

    row_append_render(&EC.row[i]);
    ++EC.document_rows;
}

void row_append_render(document_row *row) {
    int i, tabs = 0;

    for (i = 0; i < row->size; ++i)
        tabs += row->content[i] == '\t' ? 1 : 0;

    free(row->render_content);
    row->render_content = malloc(row->size + tabs * (TAB_STOP - 1) + 1);

    int render_size = 0;
    for (i = 0; i < row->size; ++i) {
        if (row->content[i] == '\t') {
            row->render_content[render_size++] = ' ';
            while (render_size % TAB_STOP != 0) row->render_content[render_size++] = ' ';
        } else {
            row->render_content[render_size++] = row->content[i];
        }
    }

    row->render_content[render_size] = '\0';
    row->render_size = render_size;
    row_set_syntax(row);
}

void row_append_string(document_row *row, char *c, size_t size) {
    row->content = realloc(row->content, row->size + size + 1);
    memcpy(&row->content[row->size], c, size);
    row->size += size;
    row->content[row->size] = '\0';
    row_append_render(row);
}

void row_free(document_row *row) {
    free(row->content);
    free(row->render_content);
    free(row->highlight);
}

void row_delete(int i) {
    if (i < 0 || i >= EC.document_rows) return;
    row_free(&EC.row[i]);
    memmove(&EC.row[i], &EC.row[i + 1], sizeof(document_row) * (EC.document_rows - i - 1));
    --EC.document_rows;
}

void row_insert_char(document_row *row, int i, int c) {
    if (i < 0 || i > row->size) i = row->size;
    row->content = realloc(row->content, row->size + 2);
    memmove(&row->content[i + 1], &row->content[i], row->size - i + 1);
    ++row->size;
    row->content[i] = c;
    row_append_render(row);
}

void insert_char(int c) {
    if (EC.cursor_y == EC.document_rows) // If cursor is at the end of the file, append a new row.
        row_append(EC.document_rows, "", 0);
    row_insert_char(&EC.row[EC.cursor_y], EC.cursor_x, c);
    ++EC.cursor_x;
}

void row_delete_char(document_row *row, int i) {
    if (i < 0 || i > row->size) return;
    memmove(&row->content[i], &row->content[i + 1], row->size - i);
    --row->size;
    row_append_render(row);
}

void delete_char() {
    if (EC.cursor_y == EC.document_rows) return;
    if (EC.cursor_x == 0 && EC.cursor_y == 0) return;

    document_row *row = &EC.row[EC.cursor_y];
    if (EC.cursor_x > 0) {
        row_delete_char(row, EC.cursor_x - 1);
        --EC.cursor_x;
    } else {
        EC.cursor_x = EC.row[EC.cursor_y - 1].size;
        row_append_string(&EC.row[EC.cursor_y - 1], row->content, row->size);
        row_delete(EC.cursor_y);
        --EC.cursor_y;
    }
}

char *rows_to_string(int *buff_size) {
    int size = 0, i;
    for (i = 0; i < EC.document_rows; ++i)
        size += EC.row[i].size + 1;
    *buff_size = size;

    char *buffer = malloc(size);
    char *current_row = buffer;
    for (i = 0; i < EC.document_rows; ++i) {
        memcpy(current_row, EC.row[i].content, EC.row[i].size);
        current_row += EC.row[i].size;
        *current_row = '\n';
        ++current_row;
    }

    return buffer;
}

void insert_new_line() {
    if (EC.cursor_x == 0) {
        row_append(EC.cursor_y, "", 0);
    } else {
        document_row *row = &EC.row[EC.cursor_y];
        row_append(EC.cursor_y + 1, &row->content[EC.cursor_x], row->size - EC.cursor_x);
        row = &EC.row[EC.cursor_y];
        row->size = EC.cursor_x;
        row->content[row->size] = '\0';
        row_append_render(row);
    }

    ++EC.cursor_y;
    EC.cursor_x = 0;
}

void open_file(char *file_name) {
    free(EC.file_name);
    EC.file_name = strdup(file_name);

    FILE *file = fopen(file_name, "r");
    if (!file) editor_exit("fopen");

    char *line = NULL;
    size_t capacity = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, file)) != -1) {
        while (size > 0 && (line[size - 1] == '\n' || line[size - 1] == '\r'))
            size--;
        row_append(EC.document_rows, line, size);
    }

    free(line);
    fclose(file);
}

void save_file() {
    if (!EC.file_name) EC.file_name = show_prompt("Save as: %s");
    if (!EC.file_name) {
        set_status("Cancelled operation!");
        return;
    }

    int size;
    char *buffer = rows_to_string(&size);
    int file = open(EC.file_name, O_RDWR | O_CREAT, 0644);
    if (file != -1) {
        if (ftruncate(file, size) != -1) {
            if (write(file, buffer, size) == size) {
                close(file);
                free(buffer);
                set_status("%d bytes written to disk", size);
                return;
            }
        }
        close(file);
    }
    free(buffer);
    set_status("Can't save! I/O error: %s", strerror(errno));
}

/*** Init ***/
int main(int argc, char *argv[]) {
    editor_enable();
    editor_init();
    if (argc >= 2) {
        open_file(argv[1]);
    }

    set_status("Ctrl + [Q-Quit, S-Save, E-Edit, C-Command, R-Read]");

    while (1) {
        refresh_screen();
        process_key();
    }
}
