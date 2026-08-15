/* path.c — runtime directory + socket path resolution with strict perms.
 *
 * Policy: $XDG_RUNTIME_DIR/agent-terminal/ if XDG_RUNTIME_DIR is set (Linux
 * convention), else ~/.agent-terminal/run/. The directory is created 0700 and
 * we HARD-FAIL if it exists with the wrong owner or with group/other bits set
 * — never silently chmod, because that would paper over a symlink/squat. */
#include "path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "xutil.h"

/* Decode one UTF-8 scalar at *pp and advance past it. Returns -1 for any
 * malformed sequence: a bad lead or continuation byte, an overlong encoding,
 * a surrogate, or anything above U+10FFFF.
 *
 * Never reads past the terminator — a NUL fails the continuation test, and
 * the loop returns before looking at the byte after it. */
static long utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned char b0 = *p;
    long cp;
    int n;
    if (b0 < 0x80) {
        cp = b0;
        n = 0;
    } else if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        n = 1;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        n = 2;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        n = 3;
    } else {
        return -1; /* a continuation byte where a lead belongs, or a 5-byte lead */
    }
    for (int i = 0; i < n; i++) {
        unsigned char b = p[1 + i];
        if ((b & 0xC0) != 0x80) return -1;
        cp = (cp << 6) | (b & 0x3F);
    }
    /* An overlong encoding is a SECOND spelling of a codepoint that already
     * has a shorter one, and a second spelling is a second name that looks
     * identical to the first. Surrogates and out-of-range values are not
     * characters at all; String::from_utf8_lossy in the GUI turns each into
     * U+FFFD, so the name it displays is not the name it would send back. */
    static const long shortest[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < shortest[n]) return -1;
    if (cp > 0x10FFFF) return -1;
    if (cp >= 0xD800 && cp <= 0xDFFF) return -1;
    *pp = p + 1 + n;
    return cp;
}

/* Codepoints that make a name misrepresent itself on screen.
 *
 * Two harms, and they are not the same shape:
 *
 * (1) REORDERING — the bidi controls. Measured in a browser engine, the
 *     shape the GUI actually is: a session named `proj<RLO>gol.hs` (RLO = U+202E)
 *     renders the kill confirmation `Kill proj<RLO>gol.hs? Its child process
 *     ends.` as `Kill proj.sdne ssecorp dlihc stI ?sh.log` — the whole sentence
 *     after the name is reversed, in the one prompt whose job is to say what
 *     is about to be destroyed. In the sidebar row the pane-count badge moves
 *     to sit inside the name. This set is CLOSED: UAX #9 defines exactly
 *     these twelve characters as the explicit formatting ones, and nothing
 *     else reorders text. Refusing them is therefore a total claim.
 *
 *     Note what is NOT here: Hebrew and Arabic LETTERS. Measured in the same
 *     four shapes, an RTL name changes nothing outside itself — neutrals next
 *     to it resolve to the paragraph direction (UAX #9 N2), so "Kill שלום?"
 *     keeps its "?" in place. CSS `unicode-bidi: isolate` was measured to be
 *     inert here for the same reason, which is why the app does not carry it.
 *
 * (2) INVISIBILITY — characters that occupy no space, so two different names
 *     look like one. `deploy`, `deploy<ZWSP>` and `dep<BOM>loy` (U+200B, U+FEFF)
 *     render to the same 40.664px in the sidebar. That matters more than it sounds: a
 *     row is what the user clicks to send keystrokes into, so a decoy row
 *     that cannot be told from the real one collects what they type. This set
 *     is OPEN — the list below is the reachable-by-hand part of it, not a
 *     completeness claim (SECURITY.md says so, and says why homoglyphs like a
 *     Cyrillic "е" cannot be dealt with at this layer at all).
 *
 * The cost is named rather than hidden: emoji that need a zero-width joiner or a
 * variation selector (a profession sequence, or a heart in its emoji
 * presentation) are not usable as session names. A name
 * is an identifier for a destructive action, and being able to tell two of
 * them apart outranks being able to spell one with an emoji. */
static bool cp_misrepresents(long cp) {
    /* Reorders — UAX #9 §2.6 explicit formatting characters. Closed set. */
    if (cp == 0x061C) return true;                 /* ALM */
    if (cp == 0x200E || cp == 0x200F) return true; /* LRM, RLM */
    if (cp >= 0x202A && cp <= 0x202E) return true; /* LRE RLE PDF LRO RLO */
    if (cp >= 0x2066 && cp <= 0x2069) return true; /* LRI RLI FSI PDI */
    /* Invisible. Open set; see the comment above. */
    if (cp == 0x00AD) return true;                 /* SOFT HYPHEN */
    if (cp == 0x034F) return true;                 /* COMBINING GRAPHEME JOINER */
    if (cp == 0x180E) return true;                 /* MONGOLIAN VOWEL SEPARATOR */
    if (cp >= 0x200B && cp <= 0x200D) return true; /* ZWSP ZWNJ ZWJ */
    if (cp >= 0x2028 && cp <= 0x2029) return true; /* LINE/PARAGRAPH SEPARATOR */
    if (cp >= 0x2060 && cp <= 0x2064) return true; /* WJ + invisible operators */
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true; /* variation selectors */
    if (cp == 0xFEFF) return true;                 /* ZWNBSP / BOM */
    if (cp >= 0xFFF9 && cp <= 0xFFFB) return true; /* interlinear annotation */
    if (cp >= 0xE0000 && cp <= 0xE007F) return true; /* tag characters */
    return false;
}

/* A session name is interpolated into a path as one component and then
 * mkdir'd, so an unvalidated name escapes the sessions directory: `../escape`
 * created ~/.agent-terminal/escape/ and a name of
 * `../../../../../../tmp/victim/pwned` wrote a scrollback log there. Names
 * that differ but resolve to the same directory ('.' and './') also shared
 * one log, so `history -s .` returned the other session's output.
 *
 * It is also a label the user reads before doing something destructive, in
 * four places that are not a terminal — the sidebar row, the kill
 * confirmation, the window title, the OS notification — so it must not be
 * able to lie about itself either (cp_misrepresents above).
 *
 * Reject rather than sanitize: rewriting a name would make `ls` disagree with
 * the directory it names, and there is no legitimate use for a separator in a
 * session name. A leading '.' is refused too, because sb_list_logs() skips
 * dotted entries — such a session would exist but never be listed. */
bool at_valid_session_name(const char *name) {
    if (!name || !*name) return false;
    if (name[0] == '.') return false; /* ".", "..", and hidden names */
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        if (*p == '/' || *p < 0x20 || *p == 0x7F) return false; /* separator, C0, DEL */
        long cp = utf8_next(&p);
        if (cp < 0) return false;
        /* C1: invisible, and a terminal may consume some as control bytes. */
        if (cp >= 0x80 && cp <= 0x9F) return false;
        if (cp_misrepresents(cp)) return false;
    }
    return true;
}

static int ensure_private_dir(const char *dir) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(dir, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (st.st_uid != getuid()) { errno = EPERM; return -1; }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) { errno = EPERM; return -1; }
    return 0;
}

int at_runtime_dir(char *out, size_t outsz) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg == '/') {
        if ((size_t)snprintf(out, outsz, "%s/agent-terminal", xdg) >= outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return ensure_private_dir(out);
    }
    const char *home = getenv("HOME");
    if (!home || *home != '/') { errno = ENOENT; return -1; }
    char base[512];
    if ((size_t)snprintf(base, sizeof base, "%s/.agent-terminal", home) >= sizeof base) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_dir(base) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/run", base) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return ensure_private_dir(out);
}

int at_socket_path(char *out, size_t outsz) {
    char dir[512];
    if (at_runtime_dir(dir, sizeof dir) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/default.sock", dir) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int at_sessions_dir(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home || *home != '/') { errno = ENOENT; return -1; }
    char base[512];
    if ((size_t)snprintf(base, sizeof base, "%s/.agent-terminal", home) >= sizeof base) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (ensure_private_dir(base) != 0) return -1;
    if ((size_t)snprintf(out, outsz, "%s/sessions", base) >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return ensure_private_dir(out);
}
