/* {{{ King James Version */
/*
 * kjv: Read the Word of God from your terminal
 *
 * License: Public domain
 **/
/* ---------------------------------------------------------------------- }}} */
/* {{{ major, minor, patch, build */

static const char* kjv_version = "1.1.1.67";

/* ---------------------------------------------------------------------- }}} */
/* {{{ include files */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <regex.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/ioctl.h>

#include "data.h"
#include "intset.h"

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_config struct */

typedef struct {
    int maximum_line_length;
    int context_before;
    int context_after;
    bool context_chapter;
    bool supress_highlights;
    bool blank_after_verse;
    bool list_books;
} kjv_config;

/* ---------------------------------------------------------------------- }}} */
/* {{{ KJV_REF definitions */

#define KJV_REF_SEARCH 1
#define KJV_REF_EXACT 2
#define KJV_REF_EXACT_SET 3
#define KJV_REF_RANGE 4
#define KJV_REF_RANGE_EXT 5

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_ref struct */

typedef struct kjv_ref {
    int type;
    unsigned int book;
    unsigned int chapter;
    unsigned int chapter_end;
    unsigned int verse;
    unsigned int verse_end;
    intset *verse_set;
    char *search_str;
    regex_t search;
} kjv_ref;

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_newref function */

static kjv_ref *
kjv_newref()
{
    return calloc(1, sizeof(kjv_ref));
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_freeref function */

static void
kjv_freeref(kjv_ref *ref)
{
    if (ref) {
        free(ref->search_str);
        regfree(&ref->search);
        free(ref);
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_boolequal function */

static bool
kjv_bookequal(const char *a, const char *b, bool short_match)
{
    for (size_t i = 0, j = 0; ; ) {
        if ((!a[i] && !b[j]) || (short_match && !b[j])) {
            return true;
        } else if (a[i] == ' ') {
            i++;
        } else if (b[j] == ' ') {
            j++;
        } else if (tolower(a[i]) != tolower(b[j])) {
            return false;
        } else {
            i++;
            j++;
        }
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_book_matches function */

static bool
kjv_book_matches(const kjv_book *book, const char *s)
{
    return
        kjv_bookequal(book->name, s, false) ||
        kjv_bookequal(book->abbr, s, false) ||
        kjv_bookequal(book->name, s, true);
}


/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_book_fromname function */

static int
kjv_book_fromname(const char *s)
{
    for (int i = 0; i < kjv_books_length; i++) {
        const kjv_book *book = &kjv_books[i];
        if (kjv_book_matches(book, s)) {
            return book->number;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_scanbook function */

static int
kjv_scanbook(const char *s, int *n)
{
    int i;
    int mode = 0;
    for (i = 0; s[i]; i++) {
        if (s[i] == ' ') {
            continue;
        } else if (('a' <= s[i] && s[i] <= 'z') || ('A' <= s[i] && s[i] <= 'Z')) {
            mode = 2;
        } else if ('0' <= s[i] && s[i] <= '9' && 0 <= mode && mode <= 1) {
            mode = 1;
        } else {
            break;
        }
    }
    *n = i;
    return mode >= 1;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_parseref function */

static int
kjv_parseref(kjv_ref *ref, const char *ref_str)
{
    // 1. <book>
    // 2. <book>:?<chapter>
    // 3. <book>:?<chapter>:<verse>
    // 3a. <book>:?<chapter>:<verse>[,<verse>]...
    // 4. <book>:?<chapter>-<chapter>
    // 5. <book>:?<chapter>:<verse>-<verse>
    // 6. <book>:?<chapter>:<verse>-<chapter>:<verse>
    // 7. /<search>
    // 8. <book>/search
    // 9. <book>:?<chapter>/search

    ref->type = 0;
    ref->book = 0;
    ref->chapter = 0;
    ref->chapter_end = 0;
    ref->verse = 0;
    ref->verse_end = 0;
    intset_free(ref->verse_set);
    ref->verse_set = NULL;
    free(ref->search_str);
    ref->search_str = NULL;
    regfree(&ref->search);

    int n = 0;
    if (kjv_scanbook(ref_str, &n) == 1) {
        // 1, 2, 3, 3a, 4, 5, 6, 8, 9
        char *bookname = strndup(ref_str, n);
        ref->book = kjv_book_fromname(bookname);
        free(bookname);
        ref_str = &ref_str[n];
    } else if (ref_str[0] == '/') {
        // 7
        goto search;
    } else {
        return 1;
    }

    if (sscanf(ref_str, ":%u%n", &ref->chapter, &n) == 1 ||
        sscanf(ref_str, "%u%n",  &ref->chapter, &n) == 1) {
        // 2, 3, 3a, 4, 5, 6, 9
        ref_str = &ref_str[n];
    } else if (ref_str[0] == '/') {
        // 8
        goto search;
    } else if (ref_str[0] == '\0') {
        // 1
        ref->type = KJV_REF_EXACT;
        return 0;
    } else {
        return 1;
    }

    if (sscanf(ref_str, ":%u%n", &ref->verse, &n) == 1) {
        // 3, 3a, 5, 6
        ref_str = &ref_str[n];
    } else if (sscanf(ref_str, "-%u%n", &ref->chapter_end, &n) == 1) {
        // 4
        if (ref_str[n] != '\0') {
            return 1;
        }
        ref->type = KJV_REF_RANGE;
        return 0;
    } else if (ref_str[0] == '/') {
        // 9
        goto search;
    } else if (ref_str[0] == '\0') {
        // 2
        ref->type = KJV_REF_EXACT;
        return 0;
    } else {
        return 1;
    }

    unsigned int value;
    int ret = sscanf(ref_str, "-%u%n", &value, &n);
    if (ret == 1 && ref_str[n] == '\0') {
        // 5
        ref->verse_end = value;
        ref->type = KJV_REF_RANGE;
        return 0;
    } else if (ret == 1) {
        // 6
        ref->chapter_end = value;
        ref_str = &ref_str[n];
    } else if (ref_str[0] == '\0') {
        // 3
        ref->type = KJV_REF_EXACT;
        return 0;
    } else if (sscanf(ref_str, ",%u%n", &value, &n) == 1) {
        // 3a
        ref->verse_set = intset_new();
        intset_add(ref->verse_set, ref->verse);
        intset_add(ref->verse_set, value);
        ref_str = &ref_str[n];
        while (true) {
            if (sscanf(ref_str, ",%u%n", &value, &n) != 1) {
                break;
            }
            intset_add(ref->verse_set, value);
            ref_str = &ref_str[n];
        }
        if (ref_str[0] != '\0') {
            return 1;
        }
        ref->type = KJV_REF_EXACT_SET;
        return 0;
    } else {
        return 1;
    }

    if (sscanf(ref_str, ":%u%n", &ref->verse_end, &n) == 1 && ref_str[n] == '\0') {
        // 6
        ref->type = KJV_REF_RANGE_EXT;
        return 0;
    } else {
        return 1;
    }

search:
    ref->type = KJV_REF_SEARCH;
    if (regcomp(&ref->search, &ref_str[1], REG_EXTENDED|REG_ICASE|REG_NOSUB) != 0) {
        return 2;
    }
    ref->search_str = strdup(&ref_str[1]);
    return 0;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ str_join function */

static char *
str_join(size_t n, char *strs[])
{
    size_t length = 0;
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            length++;
        }
        length += strlen(strs[i]);
    }
    char *str = malloc(length + 1);
    str[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            strcat(str, " ");
        }
        strcat(str, strs[i]);
    }
    return str;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_ref_search function */

static bool
kjv_ref_search(const kjv_ref *ref, const kjv_verse *verse)
{
    return (
        (
            ref->book == 0 ||
            ref->book == verse->book
        ) &&
        (
            ref->chapter == 0 ||
            verse->chapter == ref->chapter
        ) &&
        regexec(&ref->search, verse->text, 0, NULL, 0) == 0
    );
}

/* ----------------------------------------------------------------------- }}}*/
/* {{{ kjv_ref_exact function */

static bool
kjv_ref_exact(const kjv_ref *ref, const kjv_verse *verse)
{
    return (
        ref->book == verse->book &&
        (
            ref->chapter == 0 ||
            ref->chapter == verse->chapter
        ) &&
        (
            ref->verse == 0 ||
            ref->verse == verse->verse
        )
     );
}

/* ----------------------------------------------------------------------- }}}*/
/* {{{ kjv_ref_exact_set function */

static bool
kjv_ref_exact_set(const kjv_ref *ref, const kjv_verse *verse)
{
    return (
        ref->book == verse->book &&
        (
            ref->chapter == 0 ||
            verse->chapter == ref->chapter
        ) &&
        intset_contains(ref->verse_set, verse->verse)
    );
}

/* ----------------------------------------------------------------------- }}}*/
/* {{{ kjv_ref_range function */

static bool
kjv_ref_range(const kjv_ref *ref, const kjv_verse *verse)
{
    return (
        ref->book == verse->book &&
        (
            (
                ref->chapter_end == 0 &&
                ref->chapter == verse->chapter
            ) ||
            (
                verse->chapter >= ref->chapter &&
                verse->chapter <= ref->chapter_end
            )
        ) &&
        (
            ref->verse == 0 ||
            verse->verse >= ref->verse
        ) &&
        (
            ref->verse_end == 0 ||
            verse->verse <= ref->verse_end
        )
    );
}

/* ----------------------------------------------------------------------- }}}*/
/* {{{ kjv_ref_range_ext function */

static bool
kjv_ref_range_ext(const kjv_ref *ref, const kjv_verse *verse)
{
    return (
        ref->book == verse->book &&
        (
            (
                verse->chapter == ref->chapter &&
                verse->verse >= ref->verse &&
                ref->chapter != ref->chapter_end
            ) ||
            (
                verse->chapter > ref->chapter &&
                verse->chapter < ref->chapter_end
            ) ||
            (
                verse->chapter == ref->chapter_end &&
                verse->verse <= ref->verse_end &&
                ref->chapter != ref->chapter_end
            ) ||
            (
                ref->chapter == ref->chapter_end &&
                verse->chapter == ref->chapter &&
                verse->verse >= ref->verse &&
                verse->verse <= ref->verse_end
            )
        )
    );
}

/* ----------------------------------------------------------------------- }}}*/
/* {{{ kjv_verse_matches function */

static bool
kjv_verse_matches(const kjv_ref *ref, const kjv_verse *verse)
{
    switch (ref->type) {
        case KJV_REF_SEARCH:    return kjv_ref_search(ref, verse);
        case KJV_REF_EXACT:     return kjv_ref_exact(ref, verse);
        case KJV_REF_EXACT_SET: return kjv_ref_exact_set(ref, verse);
        case KJV_REF_RANGE:     return kjv_ref_range(ref, verse);
        case KJV_REF_RANGE_EXT: return kjv_ref_range_ext(ref, verse);
        default:                return false;
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ KJV_DIRECTION definitions */

#define KJV_DIRECTION_BEFORE -1
#define KJV_DIRECTION_AFTER 1

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_chapeter_bounds function */

static int
kjv_chapter_bounds(int i, int direction, int maximum_steps)
{
    assert(direction == -1 || direction == 1);

    int steps = 0;
    for ( ; 0 <= i && i < kjv_verses_length; i += direction) {
        if (maximum_steps != -1 && steps >= maximum_steps) {
            break;
        }
        if ((direction == -1 && i == 0) || (direction == 1 && i + 1 == kjv_verses_length)) {
            break;
        }
        const kjv_verse *current = &kjv_verses[i], *next = &kjv_verses[i + direction];
        if (current->book != next->book || current->chapter != next->chapter) {
            break;
        }
        steps++;
    }
    return i;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_next_match function */

static int
kjv_next_match(const kjv_ref *ref, int i)
{
    for ( ; i < kjv_verses_length; i++) {
        const kjv_verse *verse = &kjv_verses[i];
        if (kjv_verse_matches(ref, verse)) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_range struct */

typedef struct {
    int start;
    int end;
} kjv_range;

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_next_data struct */

typedef struct {
    int current;
    int next_match;
    kjv_range matches[2];
} kjv_next_data;

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_next_addrange function */

static void
kjv_next_addrange(kjv_next_data *next, kjv_range range) {
    if (next->matches[0].start == -1 && next->matches[0].end == -1) {
        next->matches[0] = range;
    } else if (range.start < next->matches[0].end) {
        next->matches[0] = range;
    } else {
        next->matches[1] = range;
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_next_verse function */

static int
kjv_next_verse(const kjv_ref *ref, const kjv_config *config, kjv_next_data *next)
{
    if (next->current >= kjv_verses_length) {
        return -1;
    }

    if (next->matches[0].start != -1 && 
        next->matches[0].end != -1 && 
        next->current >= next->matches[0].end) {

        next->matches[0] = next->matches[1];
        next->matches[1] = (kjv_range){-1, -1};
    }

    if ((next->next_match == -1 || 
         next->next_match < next->current) && 
         next->next_match < kjv_verses_length) {

        int next_match = kjv_next_match(ref, next->current);
        if (next_match >= 0) {
            next->next_match = next_match;

            kjv_range bounds = {
                .start = kjv_chapter_bounds(
                    next_match, KJV_DIRECTION_BEFORE, 
                    config->context_chapter ? -1 : config->context_before
                ),
                .end = kjv_chapter_bounds(
                    next_match, KJV_DIRECTION_AFTER,  
                    config->context_chapter ? -1 : config->context_after) + 1,
            };

            kjv_next_addrange(next, bounds);
        } else {
            next_match = kjv_verses_length;
        }
    }

    if (next->matches[0].start == -1 && next->matches[0].end == -1) {
        return -1;
    }

    if (next->current < next->matches[0].start) {
        next->current = next->matches[0].start;
    }

    return next->current++;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ ESC definitions */

#define ESC_BOLD "\033[1m"
#define ESC_UNDERLINE "\033[4m"
#define ESC_RESET "\033[m"

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_output_chap_n_verse */

static void
kjv_output_chap_n_verse(const kjv_verse *verse, FILE *f, const kjv_config *config) 
{
    char format[] = ESC_BOLD "%d:%d" ESC_RESET "\t";
    if (config->supress_highlights) {
        strcpy(format, "%d:%d ");
    }
    fprintf(f, format, verse->chapter, verse->verse);
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_output_verse_begin */

static void
kjv_output_verse_begin(FILE *f, const kjv_config *config) 
{
    if (config->supress_highlights) {
      fprintf(f, "\n");
    } else {
      fprintf(f, "\n\t");
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_output_verse_end */

static void
kjv_output_verse_end(FILE *f, const kjv_config *config) 
{
    if (config->blank_after_verse) {
      fprintf(f, "\n\n");
    } else {
      fprintf(f, "\n");
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_output_verse function */
static void
kjv_output_verse(const kjv_verse *verse, FILE *f, const kjv_config *config)
{
    kjv_output_chap_n_verse(verse, f, config);

    char verse_text[1024];
    strcpy(verse_text, verse->text);
    size_t characters_printed = 0;
    char *word = strtok(verse_text, " ");

    while (word != NULL) {
        size_t word_length = strlen(word);
        if (characters_printed + word_length +
           (characters_printed > 0 ? 1 : 0) >
            config->maximum_line_length - 8 - 2) {
            kjv_output_verse_begin(f, config);
            characters_printed = 0;
        }
        if (characters_printed > 0) {
            fprintf(f, " ");
            characters_printed++;
        }

        fprintf(f, "%s", word);
        characters_printed += word_length;
        word = strtok(NULL, " ");
    }
    kjv_output_verse_end(f, config);
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_output function */

static bool
kjv_output(const kjv_ref *ref, FILE *f, const kjv_config *config)
{
    kjv_next_data next = {
        .current = 0,
        .next_match = -1,
        .matches = {
            {-1, -1},
            {-1, -1},
        },
    };

    kjv_verse *last_printed = NULL;
    for (int verse_id; (verse_id = kjv_next_verse(ref, config, &next)) != -1; ) {
        kjv_verse *verse = &kjv_verses[verse_id];
        if (last_printed == NULL || verse->book != last_printed->book) {
            if (last_printed != NULL) {
                fprintf(f, "\n");
            }
            if (config->supress_highlights) {
                fprintf(f, "%s\n\n", kjv_books[verse->book - 1].name);
            } else {
                fprintf(f, ESC_UNDERLINE "%s" ESC_RESET "\n\n", kjv_books[verse->book - 1].name);
            }
        }
        kjv_output_verse(verse, f, config);
        last_printed = verse;
    }
    return last_printed != NULL;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_render function */

static int
kjv_render(const kjv_ref *ref, const kjv_config *config)
{
    int fds[2];
    if (pipe(fds) == -1) {
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        char *args[9] = {NULL};
        int arg = 0;
        args[arg++] = "less";
        args[arg++] = "-J";
        args[arg++] = "-I";
        if (ref->search_str != NULL) {
            args[arg++] = "-p";
            args[arg++] = ref->search_str;
        }
        args[arg++] = "-R";
        args[arg++] = "-f";
        args[arg++] = "-";
        args[arg++] = NULL;
        execvp("less", args);
        printf("unable not exec less\n");
        _exit(0);
    } else if (pid == -1) {
        printf("unable to fork\n");
        return 2;
    }
    close(fds[0]);
    FILE *output = fdopen(fds[1], "w");
    bool printed = kjv_output(ref, output, config);
    if (!printed) {
        kill(pid, SIGTERM);
    }
    fclose(output);
    waitpid(pid, NULL, 0);
    if (!printed) {
        printf("unknown reference\n");
    }
    return 0;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ usage string */

const char *
usage = "usage: kjv [flags] [reference...]\n"
"\n"
"Flags:\n"
"  -A num  number of verses of context after matching verses\n"
"  -B num  number of verses of context before matching verses\n"
"  -C      show matching verses in context of the chapter\n"
"  -b      blank line between verses.\n"
"  -d      disable highlighting.\n"
"  -h      show help\n"
"  -l      list books\n"
"  -v      show version number\n"
"  -w      maximum line width (default is 80)\n"
"\n"
"Reference:\n"
"    <Book>\n"
"        Individual book\n"
"    <Book>:<Chapter>\n"
"        Individual chapter of a book\n"
"    <Book>:<Chapter>:<Verse>[,<Verse>]...\n"
"        Individual verse(s) of a specific chapter of a book\n"
"    <Book>:<Chapter>-<Chapter>\n"
"        Range of chapters in a book\n"
"    <Book>:<Chapter>:<Verse>-<Verse>\n"
"        Range of verses in a book chapter\n"
"    <Book>:<Chapter>:<Verse>-<Chapter>:<Verse>\n"
"        Range of chapters and verses in a book\n"
"\n"
"    /<Search>\n"
"        All verses that match a pattern\n"
"    <Book>/<Search>\n"
"        All verses in a book that match a pattern\n"
"    <Book>:<Chapter>/<Search>\n"
"        All verses in a chapter of a book that match a pattern\n";

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_init_config function */

static void
kjv_init_config(kjv_config *config)
{
    config->maximum_line_length = 80;
    config->context_before = 0;
    config->context_after = 0;
    config->context_chapter = false;
    config->supress_highlights = false;
    config->blank_after_verse = false;
    config->list_books = false;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_process_command_line function */

static int
kjv_process_command_line(int argc, char *argv[], kjv_config *config)
{

    int retval = 0;

    for (int opt; (opt = getopt(argc, argv, "A:B:Cbdhlvw:")) != -1; ) {
        char *endptr;
        switch (opt) {
        case 'A':
            config->context_after = strtol(optarg, &endptr, 10);
            if (endptr[0] != '\0') {
                fprintf(stderr, "kjv: invalid flag value for -A\n\n%s", usage);
                retval = 1;
            }
            break;
        case 'B':
            config->context_before = strtol(optarg, &endptr, 10);
            if (endptr[0] != '\0') {
                fprintf(stderr, "kjv: invalid flag value for -B\n\n%s", usage);
                retval = 1;
            }
            break;
        case 'C': config->context_chapter = true;          break;
        case 'b': config->blank_after_verse = true;        break;
        case 'd': config->supress_highlights = true;           break;
        case 'l': config->list_books = true;               break;
        case 'h': printf("%s", usage); retval = 2;         break;
        case 'v': printf("%s\n", kjv_version); retval = 2; break;
        case 'w':
            config->maximum_line_length = strtol(optarg, &endptr, 10);
            if (endptr[0] != '\0') {
                fprintf(stderr, "kjv: invalid flag value for -w\n\n%s", usage);
                retval = 1;
            }
            break;
        case '?':
            fprintf(stderr, "kjv: invalid flag -%c\n\n%s", optopt, usage);
            retval = 1;
            break;
        }
    }

    if (retval == 1) { exit(1); }
    if (retval == 2) { exit(0); }
    return retval;
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_list_books funcdtion */

static void
kjv_list_books(const kjv_config *config)
{
    if (config->list_books) {
        for (int i = 0; i < kjv_books_length; i++) {
            kjv_book *book = &kjv_books[i];
            printf("%s (%s)\n", book->name, book->abbr);
        }
        exit(0);
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_line_length function */

static void
kjv_line_length(kjv_config *config)
{
    struct winsize ttysize;
    if (
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &ttysize) == 0 &&
        ttysize.ws_col > 0 &&
        config->maximum_line_length == 80
       ) {
        config->maximum_line_length = ttysize.ws_col;
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ kjv_fetch_verse function */

static void
kjv_fetch_verse(int argc, char *argv[], kjv_config *config)
{
    if (argc == optind) {
        using_history();
        while (true) {
            char *input = readline("kjv> ");
            if (input == NULL) {
                break;
            }
            add_history(input);
            kjv_ref *ref = kjv_newref();
            int success = kjv_parseref(ref, input);
            free(input);
            if (success == 0) {
                kjv_render(ref, config);
            }
            kjv_freeref(ref);
        }
    } else {
        char *ref_str = str_join(argc-optind, &argv[optind]);
        kjv_ref *ref = kjv_newref();
        int success = kjv_parseref(ref, ref_str);
        free(ref_str);
        if (success == 0) {
            kjv_render(ref, config);
        }
        kjv_freeref(ref);
    }
}

/* ---------------------------------------------------------------------- }}} */
/* {{{ main function */

int
main(int argc, char *argv[])
{
    kjv_config config;

    kjv_init_config(&config);

    kjv_process_command_line(argc, argv, &config);

    kjv_list_books(&config);

    kjv_line_length(&config);

    signal(SIGPIPE, SIG_IGN);

    kjv_fetch_verse(argc, argv, &config);

    return 0;
}

/* ---------------------------------------------------------------------- }}} */
