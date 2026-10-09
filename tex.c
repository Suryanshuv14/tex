#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** Key Definitions & Macros ***/

#define TEX_CTRL(k) ((k) & 0x1f)

enum TexKey {
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

/*** Editor State ***/

struct TexState {
  struct termios original_termios;
};

static struct TexState app;

/*** Low-Level Terminal Subsystem ***/

void texPanic(const char *reason) {
  // Clear the screen and reset cursor position so terminal state remains clean
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

  // Disable software flow control (Ctrl-S/Ctrl-Q), CR-to-NL conversion, and break/parity conditions
  raw_attrs.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // Disable post-processing of output characters
  raw_attrs.c_oflag &= ~(OPOST);

  // Configure character size to 8 bits
  raw_attrs.c_cflag |= (CS8);

  // Disable echo, canonical mode, extended input processing, and interrupt signals (Ctrl-C/Ctrl-Z)
  raw_attrs.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // Read timeout settings: non-blocking read with 100ms timeout
  raw_attrs.c_cc[VMIN] = 0;
  raw_attrs.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_attrs) == -1) {
    texPanic("tcsetattr");
  }
}

/*** Keyboard Input Parser ***/

int texReadInputKey(void) {
  int bytes_read;
  char byte_in;

  while ((bytes_read = read(STDIN_FILENO, &byte_in, 1)) != 1) {
    if (bytes_read == -1 && errno != EAGAIN) {
      texPanic("read");
    }
  }

  // Parse multi-byte ANSI escape sequences (arrows, home/end, page up/down, delete)
  if (byte_in == '\x1b') {
    char escape_seq[3];

    if (read(STDIN_FILENO, &escape_seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &escape_seq[1], 1) != 1) return '\x1b';

    if (escape_seq[0] == '[') {
      if (escape_seq[1] >= '0' && escape_seq[1] <= '9') {
        if (read(STDIN_FILENO, &escape_seq[2], 1) != 1) return '\x1b';
        if (escape_seq[2] == '~') {
          switch (escape_seq[1]) {
            case '1': return KEY_HOME;
            case '3': return KEY_DEL;
            case '4': return KEY_END;
            case '5': return KEY_PAGE_UP;
            case '6': return KEY_PAGE_DOWN;
            case '7': return KEY_HOME;
            case '8': return KEY_END;
          }
        }
      } else {
        switch (escape_seq[1]) {
          case 'A': return KEY_ARROW_UP;
          case 'B': return KEY_ARROW_DOWN;
          case 'C': return KEY_ARROW_RIGHT;
          case 'D': return KEY_ARROW_LEFT;
          case 'H': return KEY_HOME;
          case 'F': return KEY_END;
        }
      }
    } else if (escape_seq[0] == 'O') {
      switch (escape_seq[1]) {
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
      }
    }

    return '\x1b';
  }

  return byte_in;
}

/*** Event Dispatcher ***/

void texHandleKeyPress(void) {
  int key = texReadInputKey();

  switch (key) {
    case TEX_CTRL('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;
  }
}

/*** Program Entry Point ***/

int main(void) {
  texEnableRawMode();

  while (1) {
    texHandleKeyPress();
  }

  return 0;
}