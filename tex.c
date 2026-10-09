#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** Configuration & Constants ***/

#define TEX_VERSION "0.2.0"
#define TEX_TAB_STOP 4
#define TEX_QUIT_CONFIRMATIONS 2
#define TEX_CTRL(k) ((k) & 0x1f)

enum TexKey {
  KEY_BACKSPACE = 127,
  KEY_ARROW_LEFT = 1000,
  KEY_ARROW_RIGHT,
  KEY_ARROW_UP,
  KEY_ARROW_DOWN,
  KEY_DEL,
  KEY_HOME,
  KEY_END,
  KEY_PAGE_UP,
  KEY_PAGE_DOWN
};

enum TexHighlight {
  TEX_HL_NORMAL = 0,
  TEX_HL_COMMENT,
  TEX_HL_MLCOMMENT,
  TEX_HL_KEYWORD1,
  TEX_HL_KEYWORD2,
  TEX_HL_STRING,
  TEX_HL_NUMBER,
  TEX_HL_MATCH
};

#define TEX_HL_FLAG_NUMBERS (1 << 0)
#define TEX_HL_FLAG_STRINGS (1 << 1)

/*** Data Types ***/

struct TexSyntax {
  char *filetype;
  char **filematches;
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

typedef struct TexLine {
  int idx;
  int size;
  int render_size;
  char *chars;
  char *rendered;
  unsigned char *highlight;
  int has_open_comment;
} TexLine;

struct TexState {
  int cursor_x, cursor_y;     // File cursor coordinate
  int render_x;               // Render column coordinate (for tabs)
  int row_offset;             // Vertical viewport scroll offset
  int col_offset;             // Horizontal viewport scroll offset
  int screen_rows;            // Screen row height (usable for text)
  int screen_cols;            // Screen column width
  int num_lines;              // Total lines in document
  TexLine *lines;             // Array of lines
  int is_modified;            // Dirty flag (unsaved edits)
  char *filepath;             // Path of active file
  char status_message[80];    // Ephemeral message bar content
  time_t status_message_time; // Time when message was posted
  struct TexSyntax *syntax;   // Active syntax highlighting rules
  struct termios original_termios;
};

static struct TexState app;

/*** Syntax Highlighting Rules Database ***/

char *C_HL_extensions[] = {".c", ".h", ".cpp", ".cc", ".hpp", NULL};
char *C_HL_keywords[] = {
    "switch",    "if",        "while",     "for",       "break",    "continue",
    "return",    "else",      "struct",    "union",     "typedef",  "static",
    "enum",      "class",     "case",      "const",     "sizeof",   "volatile",
    "register",  "int|",      "long|",     "double|",   "float|",   "char|",
    "unsigned|", "signed|",   "void|",     "size_t|",   "ssize_t|", "bool|",
    "uint8_t|",  "uint16_t|", "uint32_t|", "uint64_t|", "int8_t|",  "int16_t|",
    "int32_t|",  "int64_t|",  NULL};

struct TexSyntax HLDB[] = {
    {"c", C_HL_extensions, C_HL_keywords, "//", "/*", "*/",
     TEX_HL_FLAG_NUMBERS | TEX_HL_FLAG_STRINGS},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Function Prototypes ***/

void texSetStatusMessage(const char *fmt, ...);
void texRenderScreen(void);
char *texPromptInput(char *prompt, void (*callback)(char *, int));

/*** Dynamic Screen Buffer Subsystem ***/

struct ScreenBuffer {
  char *data;
  int length;
};

#define SCREEN_BUFFER_INIT {NULL, 0}

void screenBufferAppend(struct ScreenBuffer *sb, const char *s, int len) {
  char *new_buf = realloc(sb->data, sb->length + len);
  if (new_buf == NULL)
    return;
  memcpy(&new_buf[sb->length], s, len);
  sb->data = new_buf;
  sb->length += len;
}

void screenBufferFree(struct ScreenBuffer *sb) { free(sb->data); }

/*** Low-Level Terminal Control ***/

void texPanic(const char *reason) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(reason);
  exit(1);
}

void texDisableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &app.original_termios) == -1) {
    texPanic("tcsetattr");
  }
}

void texEnableRawMode(void) {
  if (tcgetattr(STDIN_FILENO, &app.original_termios) == -1) {
    texPanic("tcgetattr");
  }
  atexit(texDisableRawMode);

  struct termios raw_attrs = app.original_termios;
  raw_attrs.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw_attrs.c_oflag &= ~(OPOST);
  raw_attrs.c_cflag |= (CS8);
  raw_attrs.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw_attrs.c_cc[VMIN] = 0;
  raw_attrs.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_attrs) == -1) {
    texPanic("tcsetattr");
  }
}

int texReadInputKey(void) {
  int bytes_read;
  char byte_in;

  while ((bytes_read = read(STDIN_FILENO, &byte_in, 1)) != 1) {
    if (bytes_read == -1 && errno != EAGAIN)
      texPanic("read");
  }

  if (byte_in == '\x1b') {
    char escape_seq[3];

    if (read(STDIN_FILENO, &escape_seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &escape_seq[1], 1) != 1)
      return '\x1b';

    if (escape_seq[0] == '[') {
      if (escape_seq[1] >= '0' && escape_seq[1] <= '9') {
        if (read(STDIN_FILENO, &escape_seq[2], 1) != 1)
          return '\x1b';
        if (escape_seq[2] == '~') {
          switch (escape_seq[1]) {
          case '1':
            return KEY_HOME;
          case '3':
            return KEY_DEL;
          case '4':
            return KEY_END;
          case '5':
            return KEY_PAGE_UP;
          case '6':
            return KEY_PAGE_DOWN;
          case '7':
            return KEY_HOME;
          case '8':
            return KEY_END;
          }
        }
      } else {
        switch (escape_seq[1]) {
        case 'A':
          return KEY_ARROW_UP;
        case 'B':
          return KEY_ARROW_DOWN;
        case 'C':
          return KEY_ARROW_RIGHT;
        case 'D':
          return KEY_ARROW_LEFT;
        case 'H':
          return KEY_HOME;
        case 'F':
          return KEY_END;
        }
      }
    } else if (escape_seq[0] == 'O') {
      switch (escape_seq[1]) {
      case 'H':
        return KEY_HOME;
      case 'F':
        return KEY_END;
      }
    }
    return '\x1b';
  }

  return byte_in;
}

int texGetCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return 0;
}

int texGetWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col != 0) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }

  // Fallback 1: Query cursor position
  if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) == 12) {
    if (texGetCursorPosition(rows, cols) == 0) {
      return 0;
    }
  }

  // Fallback 2: Standard default terminal dimensions
  *cols = 80;
  *rows = 24;
  return 0;
}

/*** Syntax Highlighting Engine ***/

int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void texUpdateSyntax(TexLine *line) {
  line->highlight = realloc(line->highlight, line->render_size);
  memset(line->highlight, TEX_HL_NORMAL, line->render_size);

  if (app.syntax == NULL)
    return;

  char **keywords = app.syntax->keywords;
  char *scs = app.syntax->singleline_comment_start;
  char *mcs = app.syntax->multiline_comment_start;
  char *mce = app.syntax->multiline_comment_end;

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  int prev_sep = 1;
  int in_string = 0;
  int in_comment = (line->idx > 0 && app.lines[line->idx - 1].has_open_comment);

  int i = 0;
  while (i < line->render_size) {
    char c = line->rendered[i];
    unsigned char prev_hl = (i > 0) ? line->highlight[i - 1] : TEX_HL_NORMAL;

    // Single-line comments
    if (scs_len && !in_string && !in_comment) {
      if (!strncmp(&line->rendered[i], scs, scs_len)) {
        memset(&line->highlight[i], TEX_HL_COMMENT, line->render_size - i);
        break;
      }
    }

    // Multi-line comments
    if (mcs_len && mce_len && !in_string) {
      if (in_comment) {
        line->highlight[i] = TEX_HL_MLCOMMENT;
        if (!strncmp(&line->rendered[i], mce, mce_len)) {
          memset(&line->highlight[i], TEX_HL_MLCOMMENT, mce_len);
          i += mce_len;
          in_comment = 0;
          prev_sep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&line->rendered[i], mcs, mcs_len)) {
        memset(&line->highlight[i], TEX_HL_MLCOMMENT, mcs_len);
        i += mcs_len;
        in_comment = 1;
        continue;
      }
    }

    // String literals
    if (app.syntax->flags & TEX_HL_FLAG_STRINGS) {
      if (in_string) {
        line->highlight[i] = TEX_HL_STRING;
        if (c == '\\' && i + 1 < line->render_size) {
          line->highlight[i + 1] = TEX_HL_STRING;
          i += 2;
          continue;
        }
        if (c == in_string)
          in_string = 0;
        i++;
        prev_sep = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          in_string = c;
          line->highlight[i] = TEX_HL_STRING;
          i++;
          continue;
        }
      }
    }

    // Numbers
    if (app.syntax->flags & TEX_HL_FLAG_NUMBERS) {
      if ((isdigit(c) && (prev_sep || prev_hl == TEX_HL_NUMBER)) ||
          (c == '.' && prev_hl == TEX_HL_NUMBER)) {
        line->highlight[i] = TEX_HL_NUMBER;
        i++;
        prev_sep = 0;
        continue;
      }
    }

    // Keywords
    if (prev_sep) {
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen = strlen(keywords[j]);
        int is_type = keywords[j][klen - 1] == '|';
        if (is_type)
          klen--;

        if (!strncmp(&line->rendered[i], keywords[j], klen) &&
            is_separator(line->rendered[i + klen])) {
          memset(&line->highlight[i],
                 is_type ? TEX_HL_KEYWORD2 : TEX_HL_KEYWORD1, klen);
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    prev_sep = is_separator(c);
    i++;
  }

  int changed = (line->has_open_comment != in_comment);
  line->has_open_comment = in_comment;
  if (changed && line->idx + 1 < app.num_lines) {
    texUpdateSyntax(&app.lines[line->idx + 1]);
  }
}

int texSyntaxToColor(int hl) {
  switch (hl) {
  case TEX_HL_COMMENT:
  case TEX_HL_MLCOMMENT:
    return 36; // Cyan
  case TEX_HL_KEYWORD1:
    return 33; // Yellow
  case TEX_HL_KEYWORD2:
    return 32; // Green
  case TEX_HL_STRING:
    return 35; // Magenta
  case TEX_HL_NUMBER:
    return 31; // Red
  case TEX_HL_MATCH:
    return 34; // Blue
  default:
    return 37; // White
  }
}

void texSelectSyntax(void) {
  app.syntax = NULL;
  if (app.filepath == NULL)
    return;

  char *ext = strrchr(app.filepath, '.');

  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    struct TexSyntax *s = &HLDB[j];
    unsigned int i = 0;
    while (s->filematches[i]) {
      int is_ext = (s->filematches[i][0] == '.');
      if ((is_ext && ext && !strcmp(ext, s->filematches[i])) ||
          (!is_ext && strstr(app.filepath, s->filematches[i]))) {
        app.syntax = s;

        for (int filerow = 0; filerow < app.num_lines; filerow++) {
          texUpdateSyntax(&app.lines[filerow]);
        }
        return;
      }
      i++;
    }
  }
}

/*** Line Operations & Coordinates ***/

int texLineCxToRx(TexLine *line, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (line->chars[j] == '\t') {
      rx += (TEX_TAB_STOP - 1) - (rx % TEX_TAB_STOP);
    }
    rx++;
  }
  return rx;
}

int texLineRxToCx(TexLine *line, int rx) {
  int cur_rx = 0;
  int cx;
  for (cx = 0; cx < line->size; cx++) {
    if (line->chars[cx] == '\t') {
      cur_rx += (TEX_TAB_STOP - 1) - (cur_rx % TEX_TAB_STOP);
    }
    cur_rx++;
    if (cur_rx > rx)
      return cx;
  }
  return cx;
}

void texUpdateLine(TexLine *line) {
  int tabs = 0;
  for (int j = 0; j < line->size; j++) {
    if (line->chars[j] == '\t')
      tabs++;
  }

  free(line->rendered);
  line->rendered = malloc(line->size + tabs * (TEX_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < line->size; j++) {
    if (line->chars[j] == '\t') {
      line->rendered[idx++] = ' ';
      while (idx % TEX_TAB_STOP != 0)
        line->rendered[idx++] = ' ';
    } else {
      line->rendered[idx++] = line->chars[j];
    }
  }
  line->rendered[idx] = '\0';
  line->render_size = idx;

  texUpdateSyntax(line);
}

void texInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > app.num_lines)
    return;

  app.lines = realloc(app.lines, sizeof(TexLine) * (app.num_lines + 1));
  memmove(&app.lines[at + 1], &app.lines[at],
          sizeof(TexLine) * (app.num_lines - at));

  for (int j = at + 1; j <= app.num_lines; j++)
    app.lines[j].idx++;

  app.lines[at].idx = at;
  app.lines[at].size = len;
  app.lines[at].chars = malloc(len + 1);
  memcpy(app.lines[at].chars, s, len);
  app.lines[at].chars[len] = '\0';

  app.lines[at].render_size = 0;
  app.lines[at].rendered = NULL;
  app.lines[at].highlight = NULL;
  app.lines[at].has_open_comment = 0;

  texUpdateLine(&app.lines[at]);

  app.num_lines++;
  app.is_modified++;
}

void texFreeLine(TexLine *line) {
  free(line->chars);
  free(line->rendered);
  free(line->highlight);
}

void texDeleteRow(int at) {
  if (at < 0 || at >= app.num_lines)
    return;

  texFreeLine(&app.lines[at]);
  memmove(&app.lines[at], &app.lines[at + 1],
          sizeof(TexLine) * (app.num_lines - at - 1));

  for (int j = at; j < app.num_lines - 1; j++)
    app.lines[j].idx--;

  app.num_lines--;
  app.is_modified++;
}

void texLineInsertChar(TexLine *line, int at, int c) {
  if (at < 0 || at > line->size)
    at = line->size;
  line->chars = realloc(line->chars, line->size + 2);
  memmove(&line->chars[at + 1], &line->chars[at], line->size - at + 1);
  line->size++;
  line->chars[at] = c;
  texUpdateLine(line);
  app.is_modified++;
}

void texLineAppendString(TexLine *line, char *s, size_t len) {
  line->chars = realloc(line->chars, line->size + len + 1);
  memcpy(&line->chars[line->size], s, len);
  line->size += len;
  line->chars[line->size] = '\0';
  texUpdateLine(line);
  app.is_modified++;
}

void texLineDeleteChar(TexLine *line, int at) {
  if (at < 0 || at >= line->size)
    return;
  memmove(&line->chars[at], &line->chars[at + 1], line->size - at);
  line->size--;
  texUpdateLine(line);
  app.is_modified++;
}

/*** Editor Actions (Insert, Delete, Newline) ***/

void texInsertChar(int c) {
  if (app.cursor_y == app.num_lines) {
    texInsertRow(app.num_lines, "", 0);
  }
  texLineInsertChar(&app.lines[app.cursor_y], app.cursor_x, c);
  app.cursor_x++;
}

void texInsertNewline(void) {
  if (app.cursor_x == 0) {
    texInsertRow(app.cursor_y, "", 0);
  } else {
    TexLine *row = &app.lines[app.cursor_y];
    texInsertRow(app.cursor_y + 1, &row->chars[app.cursor_x],
                 row->size - app.cursor_x);
    row = &app.lines[app.cursor_y];
    row->size = app.cursor_x;
    row->chars[row->size] = '\0';
    texUpdateLine(row);
  }
  app.cursor_y++;
  app.cursor_x = 0;
}

void texDeleteChar(void) {
  if (app.cursor_y == app.num_lines)
    return;
  if (app.cursor_x == 0 && app.cursor_y == 0)
    return;

  TexLine *row = &app.lines[app.cursor_y];
  if (app.cursor_x > 0) {
    texLineDeleteChar(row, app.cursor_x - 1);
    app.cursor_x--;
  } else {
    app.cursor_x = app.lines[app.cursor_y - 1].size;
    texLineAppendString(&app.lines[app.cursor_y - 1], row->chars, row->size);
    texDeleteRow(app.cursor_y);
    app.cursor_y--;
  }
}

/*** File I/O & Persistence ***/

char *texLinesToString(int *buflen) {
  int total_len = 0;
  for (int j = 0; j < app.num_lines; j++) {
    total_len += app.lines[j].size + 1;
  }
  *buflen = total_len;

  char *buf = malloc(total_len);
  char *p = buf;
  for (int j = 0; j < app.num_lines; j++) {
    memcpy(p, app.lines[j].chars, app.lines[j].size);
    p += app.lines[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void texOpenFile(const char *filepath) {
  free(app.filepath);
  app.filepath = strdup(filepath);

  texSelectSyntax();

  FILE *fp = fopen(filepath, "r");
  if (!fp) {
    // If file doesn't exist, start with clean empty buffer
    return;
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
      linelen--;
    }
    texInsertRow(app.num_lines, line, linelen);
  }

  free(line);
  fclose(fp);
  app.is_modified = 0;
}

void texSaveFile(void) {
  if (app.filepath == NULL) {
    app.filepath = texPromptInput("Save as: %s (ESC to cancel)", NULL);
    if (app.filepath == NULL) {
      texSetStatusMessage("Save aborted");
      return;
    }
    texSelectSyntax();
  }

  int len;
  char *buf = texLinesToString(&len);

  int fd = open(app.filepath, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        app.is_modified = 0;
        texSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  texSetStatusMessage("Error saving file: %s", strerror(errno));
}

/*** Search Subsystem ***/

void texSearchCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;
  static char *saved_hl = NULL;

  if (saved_hl) {
    memcpy(app.lines[saved_hl_line].highlight, saved_hl,
           app.lines[saved_hl_line].render_size);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == KEY_ARROW_RIGHT || key == KEY_ARROW_DOWN) {
    direction = 1;
  } else if (key == KEY_ARROW_LEFT || key == KEY_ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;
  int current = last_match;

  for (int i = 0; i < app.num_lines; i++) {
    current += direction;
    if (current == -1)
      current = app.num_lines - 1;
    else if (current == app.num_lines)
      current = 0;

    TexLine *row = &app.lines[current];
    char *match = strstr(row->rendered, query);
    if (match) {
      last_match = current;
      app.cursor_y = current;
      app.cursor_x = texLineRxToCx(row, match - row->rendered);
      app.row_offset = app.num_lines;

      saved_hl_line = current;
      saved_hl = malloc(row->render_size);
      memcpy(saved_hl, row->highlight, row->render_size);
      memset(&row->highlight[match - row->rendered], TEX_HL_MATCH,
             strlen(query));
      break;
    }
  }
}

void texSearch(void) {
  int saved_cx = app.cursor_x;
  int saved_cy = app.cursor_y;
  int saved_coloff = app.col_offset;
  int saved_rowoff = app.row_offset;

  char *query =
      texPromptInput("Search: %s (Use ESC/Arrows/Enter)", texSearchCallback);

  if (query) {
    free(query);
  } else {
    app.cursor_x = saved_cx;
    app.cursor_y = saved_cy;
    app.col_offset = saved_coloff;
    app.row_offset = saved_rowoff;
  }
}

/*** Interactive User Prompts ***/

char *texPromptInput(char *prompt, void (*callback)(char *, int)) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    texSetStatusMessage(prompt, buf);
    texRenderScreen();

    int c = texReadInputKey();
    if (c == KEY_DEL || c == TEX_CTRL('h') || c == KEY_BACKSPACE) {
      if (buflen != 0)
        buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      texSetStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (buflen != 0) {
        texSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }

    if (callback)
      callback(buf, c);
  }
}

/*** Viewport Scrolling & Rendering ***/

void texScroll(void) {
  app.render_x = 0;
  if (app.cursor_y < app.num_lines) {
    app.render_x = texLineCxToRx(&app.lines[app.cursor_y], app.cursor_x);
  }

  if (app.cursor_y < app.row_offset) {
    app.row_offset = app.cursor_y;
  }
  if (app.cursor_y >= app.row_offset + app.screen_rows) {
    app.row_offset = app.cursor_y - app.screen_rows + 1;
  }
  if (app.render_x < app.col_offset) {
    app.col_offset = app.render_x;
  }
  if (app.render_x >= app.col_offset + app.screen_cols) {
    app.col_offset = app.render_x - app.screen_cols + 1;
  }
}

void texRenderRows(struct ScreenBuffer *sb) {
  for (int y = 0; y < app.screen_rows; y++) {
    int filerow = y + app.row_offset;
    if (filerow >= app.num_lines) {
      if (app.num_lines == 0 && y == app.screen_rows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Tex Editor -- Version %s", TEX_VERSION);
        if (welcomelen > app.screen_cols)
          welcomelen = app.screen_cols;
        int padding = (app.screen_cols - welcomelen) / 2;
        if (padding) {
          screenBufferAppend(sb, "~", 1);
          padding--;
        }
        while (padding--)
          screenBufferAppend(sb, " ", 1);
        screenBufferAppend(sb, welcome, welcomelen);
      } else {
        screenBufferAppend(sb, "~", 1);
      }
    } else {
      int len = app.lines[filerow].render_size - app.col_offset;
      if (len < 0)
        len = 0;
      if (len > app.screen_cols)
        len = app.screen_cols;

      char *c = &app.lines[filerow].rendered[app.col_offset];
      unsigned char *hl = &app.lines[filerow].highlight[app.col_offset];
      int current_color = -1;

      for (int j = 0; j < len; j++) {
        if (iscntrl(c[j])) {
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';
          screenBufferAppend(sb, "\x1b[7m", 4);
          screenBufferAppend(sb, &sym, 1);
          screenBufferAppend(sb, "\x1b[m", 3);
          if (current_color != -1) {
            char color_buf[16];
            int clen = snprintf(color_buf, sizeof(color_buf), "\x1b[%dm",
                                current_color);
            screenBufferAppend(sb, color_buf, clen);
          }
        } else if (hl[j] == TEX_HL_NORMAL) {
          if (current_color != -1) {
            screenBufferAppend(sb, "\x1b[39m", 5);
            current_color = -1;
          }
          screenBufferAppend(sb, &c[j], 1);
        } else {
          int color = texSyntaxToColor(hl[j]);
          if (color != current_color) {
            current_color = color;
            char color_buf[16];
            int clen =
                snprintf(color_buf, sizeof(color_buf), "\x1b[%dm", color);
            screenBufferAppend(sb, color_buf, clen);
          }
          screenBufferAppend(sb, &c[j], 1);
        }
      }
      screenBufferAppend(sb, "\x1b[39m", 5);
    }

    screenBufferAppend(sb, "\x1b[K", 3);
    screenBufferAppend(sb, "\r\n", 2);
  }
}

void texRenderStatusBar(struct ScreenBuffer *sb) {
  screenBufferAppend(sb, "\x1b[7m", 4); // Invert colors for status bar

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     app.filepath ? app.filepath : "[No Name]", app.num_lines,
                     app.is_modified ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
                      app.syntax ? app.syntax->filetype : "no ft",
                      app.cursor_y + 1, app.num_lines);

  if (len > app.screen_cols)
    len = app.screen_cols;
  screenBufferAppend(sb, status, len);

  while (len < app.screen_cols) {
    if (app.screen_cols - len == rlen) {
      screenBufferAppend(sb, rstatus, rlen);
      break;
    } else {
      screenBufferAppend(sb, " ", 1);
      len++;
    }
  }
  screenBufferAppend(sb, "\x1b[m", 3);
  screenBufferAppend(sb, "\r\n", 2);
}

void texRenderMessageBar(struct ScreenBuffer *sb) {
  screenBufferAppend(sb, "\x1b[K", 3);
  int msglen = strlen(app.status_message);
  if (msglen > app.screen_cols)
    msglen = app.screen_cols;
  if (msglen && time(NULL) - app.status_message_time < 5) {
    screenBufferAppend(sb, app.status_message, msglen);
  }
}

void texRenderScreen(void) {
  texScroll();

  struct ScreenBuffer sb = SCREEN_BUFFER_INIT;

  screenBufferAppend(&sb, "\x1b[?25l", 6); // Hide cursor during rendering
  screenBufferAppend(&sb, "\x1b[H", 3);    // Reposition cursor to top-left

  texRenderRows(&sb);
  texRenderStatusBar(&sb);
  texRenderMessageBar(&sb);

  // Position cursor to exact position
  char cursor_pos[32];
  snprintf(cursor_pos, sizeof(cursor_pos), "\x1b[%d;%dH",
           (app.cursor_y - app.row_offset) + 1,
           (app.render_x - app.col_offset) + 1);
  screenBufferAppend(&sb, cursor_pos, strlen(cursor_pos));

  screenBufferAppend(&sb, "\x1b[?25h", 6); // Restore cursor visibility

  write(STDOUT_FILENO, sb.data, sb.length);
  screenBufferFree(&sb);
}

void texSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(app.status_message, sizeof(app.status_message), fmt, ap);
  va_end(ap);
  app.status_message_time = time(NULL);
}

/*** Cursor Navigation ***/

void texNavigateCursor(int key) {
  TexLine *row =
      (app.cursor_y >= app.num_lines) ? NULL : &app.lines[app.cursor_y];

  switch (key) {
  case KEY_ARROW_LEFT:
    if (app.cursor_x != 0) {
      app.cursor_x--;
    } else if (app.cursor_y > 0) {
      app.cursor_y--;
      app.cursor_x = app.lines[app.cursor_y].size;
    }
    break;
  case KEY_ARROW_RIGHT:
    if (row && app.cursor_x < row->size) {
      app.cursor_x++;
    } else if (row && app.cursor_x == row->size) {
      app.cursor_y++;
      app.cursor_x = 0;
    }
    break;
  case KEY_ARROW_UP:
    if (app.cursor_y != 0) {
      app.cursor_y--;
    }
    break;
  case KEY_ARROW_DOWN:
    if (app.cursor_y < app.num_lines) {
      app.cursor_y++;
    }
    break;
  }

  row = (app.cursor_y >= app.num_lines) ? NULL : &app.lines[app.cursor_y];
  int rowlen = row ? row->size : 0;
  if (app.cursor_x > rowlen) {
    app.cursor_x = rowlen;
  }
}

/*** Event Dispatcher ***/

void texHandleKeyPress(void) {
  static int quit_times = TEX_QUIT_CONFIRMATIONS;

  int key = texReadInputKey();

  switch (key) {
  case '\r':
    texInsertNewline();
    break;

  case TEX_CTRL('q'):
    if (app.is_modified && quit_times > 0) {
      texSetStatusMessage("WARNING: File has unsaved changes! Press Ctrl-Q %d "
                          "more time%s to discard.",
                          quit_times, quit_times > 1 ? "s" : "");
      quit_times--;
      return;
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case TEX_CTRL('s'):
    texSaveFile();
    break;

  case KEY_HOME:
    app.cursor_x = 0;
    break;

  case KEY_END:
    if (app.cursor_y < app.num_lines) {
      app.cursor_x = app.lines[app.cursor_y].size;
    }
    break;

  case TEX_CTRL('f'):
    texSearch();
    break;

  case KEY_BACKSPACE:
  case TEX_CTRL('h'):
  case KEY_DEL:
    if (key == KEY_DEL)
      texNavigateCursor(KEY_ARROW_RIGHT);
    texDeleteChar();
    break;

  case KEY_PAGE_UP:
  case KEY_PAGE_DOWN: {
    if (key == KEY_PAGE_UP) {
      app.cursor_y = app.row_offset;
    } else if (key == KEY_PAGE_DOWN) {
      app.cursor_y = app.row_offset + app.screen_rows - 1;
      if (app.cursor_y > app.num_lines)
        app.cursor_y = app.num_lines;
    }

    int times = app.screen_rows;
    while (times--) {
      texNavigateCursor(key == KEY_PAGE_UP ? KEY_ARROW_UP : KEY_ARROW_DOWN);
    }
  } break;

  case KEY_ARROW_UP:
  case KEY_ARROW_DOWN:
  case KEY_ARROW_LEFT:
  case KEY_ARROW_RIGHT:
    texNavigateCursor(key);
    break;

  case TEX_CTRL('l'):
  case '\x1b':
    break;

  default:
    texInsertChar(key);
    break;
  }

  quit_times = TEX_QUIT_CONFIRMATIONS;
}

/*** Initialization Subsystem ***/

void texInitEditor(void) {
  app.cursor_x = 0;
  app.cursor_y = 0;
  app.render_x = 0;
  app.row_offset = 0;
  app.col_offset = 0;
  app.num_lines = 0;
  app.lines = NULL;
  app.is_modified = 0;
  app.filepath = NULL;
  app.status_message[0] = '\0';
  app.status_message_time = 0;
  app.syntax = NULL;

  if (texGetWindowSize(&app.screen_rows, &app.screen_cols) == -1) {
    texPanic("texGetWindowSize");
  }
  // Reserve 2 rows at the bottom: 1 for status bar, 1 for message bar
  app.screen_rows -= 2;
}

int main(int argc, char *argv[]) {
  texEnableRawMode();
  texInitEditor();

  if (argc >= 2) {
    texOpenFile(argv[1]);
  }

  texSetStatusMessage("HELP: Ctrl-S = Save | Ctrl-Q = Quit | Ctrl-F = Find");

  while (1) {
    texRenderScreen();
    texHandleKeyPress();
  }

  return 0;
}