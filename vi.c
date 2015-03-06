#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>

/* Terminal configuration data */
static struct termios term_orig, term_config;
static int termdesc = -1;

/* Current editing locations (screen and file) */
static char crsr_x, crsr_y, cur_line, cur_pos;

/* Total number of lines allocated */
static int line_count = 0;

/* Text is stored line-by-line. Line lengths are stored with the text data
 * to avoid lots of strlen() calls for line wrapping, insertion, etc. */
struct line {
	struct line *prev;
	struct line *next;
	char *text;
	int len;
};
static struct line *line_head = NULL;

/* When a line is being edited, store the edits in a temporary buffer.
 * Backspace/delete operations will have a negative length. This buffer
 * is committed on cursor movement, leaving edit mode, or hitting the
 * end of the temporary buffer. */
#define TEMP_BUFSIZ 128
struct {
	int insert_pos;
	int len;
	char text[TEMP_BUFSIZ];
} temp_line;

const char initial_line[] = "Now is the time for all good men to come to the aid of the party";
const int rows=25;
const int cols=80;

/* Escape sequence function definitions */
#define CLEAR_SCREEN() write(STDOUT_FILENO, "\033[H\033[J", 6);
#define ERASE_LINE() write(STDOUT_FILENO, "\033[2K", 4);
#define CRSR_UP() write(STDOUT_FILENO, "\033[1A", 4);
#define CRSR_DOWN() write(STDOUT_FILENO, "\033[1B", 4);
#define CRSR_LEFT() write(STDOUT_FILENO, "\033[1C", 4);
#define CRSR_RIGHT() write(STDOUT_FILENO, "\033[1D", 4);
#define DISABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7l", 4);
#define ENABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7h", 4);

/* Walk the line list to the requested line */
static inline struct line *walk_to_line(int num)
{
	struct line *line = line_head;
	int i = 0;

	while (line != NULL) {
		if (i == num) break;
		line = line->next;
	}
	return line;
}

/* Allocate a new line after the selected line */
static struct line *new_line(unsigned int start, const char * restrict text)
{
	struct line *cur_line, *new_line;

	/* Cannot open lines out of current range */
	if (start > line_count) return NULL;

	if (start > 0) cur_line = walk_to_line(start);
	else cur_line = line_head;

	/* Insert a new line */
	new_line = (struct line *)malloc(sizeof(struct line));
	if (!new_line) goto oom;
	if (cur_line == NULL) {
		/* If line_head is NULL, no lines exist yet */
		line_head = new_line;
		new_line->next = NULL;
		new_line->prev = NULL;
	} else {
		new_line->next = cur_line->next;
		new_line->prev = cur_line;
		cur_line->next = new_line;
	}
	if (new_line->next != NULL) new_line->next->prev = new_line;
	/* Allocate the text area (if applicable) */
	if (text == NULL) {
		new_line->text = NULL;
		new_line->len = 0;
	} else {
		new_line->len = strlen(text);
		new_line->text = (char *)malloc(new_line->len + 1);
		if (!new_line->text) goto oom;
		strncpy(new_line->text, text, new_line->len);
	}

	return new_line;
oom:
	fprintf(stderr, "out of memory\n");
	exit(EXIT_FAILURE);
}

/* Destroy and free one line in the line list */
static int destroy_line(int num)
{
	struct line *line;

	line = walk_to_line(num);
	if (line == NULL) return -1;
	if (line->text != NULL) free(line->text);
	line->prev->next = line->next;
	free(line);

	return 0;
}

static void destroy_buffer(void)
{
	struct line *line = line_head;
	struct line *prev = NULL;

	line_head = NULL;

	/* Free lines in order until list is exhausted */
	while (line != NULL) {
		if (line->text != NULL) free(line->text);
		if (line->prev != NULL) free(line->prev);
		prev = line;
		line = line->next;
	}
	/* Free the final line, if applicable */
	if (prev != NULL) free(prev);
}

/* Restore terminal to original configuration */
static void term_restore(void)
{
	if (termdesc != -1) tcsetattr(termdesc, TCSANOW, &term_orig);
	ENABLE_LINE_WRAP();
	return;
}

/* Initialize terminal settings */
static int term_init(void)
{
	/* Only init terminal once */
	if (termdesc != -1) return 0;

	/* Find a std* stream with a TTY */
	if (isatty(STDIN_FILENO)) termdesc = STDIN_FILENO;
	else if (isatty(STDOUT_FILENO)) termdesc = STDOUT_FILENO;
	else if (isatty(STDERR_FILENO)) termdesc = STDERR_FILENO;
	else return -ENOTTY;

	/* Get current terminal configuration and save it*/
	if (tcgetattr(termdesc, &term_orig)) return -EBADF;
	memcpy(&term_config, &term_orig, sizeof(struct termios));

	/* Disable buffering */
	if (isatty(STDIN_FILENO)) setvbuf(stdin, NULL, _IONBF, 0);
	if (isatty(STDOUT_FILENO)) setvbuf(stdout, NULL, _IONBF, 0);
	if (isatty(STDERR_FILENO)) setvbuf(stderr, NULL, _IONBF, 0);

	/* Configure terminal settings */

	/* c_cc */
	term_config.c_cc[VTIME] = 0;
	term_config.c_cc[VMIN] = 1;	/* read() one char at a time*/

	/* iflag */
#ifdef IUCLC
	term_config.c_iflag &= ~IUCLC;
#endif
	term_config.c_iflag &= ~PARMRK;
	term_config.c_iflag |= IGNPAR;
	term_config.c_iflag &= ~IGNBRK;
	term_config.c_iflag |= BRKINT;
	term_config.c_iflag &= ~ISTRIP;
	term_config.c_iflag &= ~(INLCR | IGNCR | ICRNL);

	/* cflag */
	term_config.c_cflag &= ~CSIZE;
	term_config.c_cflag |= CS8;
	term_config.c_cflag |= CREAD;

	/* lflag */
	term_config.c_lflag |= ISIG;
	term_config.c_lflag &= ~ICANON;	/* disable line buffering */
	term_config.c_lflag &= ~IEXTEN;

	/* disable local echo */
	term_config.c_lflag &= ~(ECHO | ECHONL | ECHOE | ECHOK);

	/* Finalize settings */
	tcsetattr(termdesc, TCSANOW, &term_config);

	/* Disable automatic line wrapping */
	DISABLE_LINE_WRAP();

	return 0;
}


int main(int argc, char **argv)
{
	struct line *cur_line;
	int i;

	if ((i = term_init()) != 0) {
		if (i == -ENOTTY) fprintf(stderr, "tty is required\n");
		else fprintf(stderr, "cannot init terminal: %s\n", strerror(-i));
		exit(EXIT_FAILURE);
	}

	CLEAR_SCREEN();
	cur_line = new_line(0, NULL);
	i = getc(stdin);
	printf("You said %c\n", i);

	term_restore();
	destroy_buffer();
	exit(EXIT_SUCCESS);
}
